/* DVDREADFS: mount dvd's using libdvdread. */
/* Sam Horrocks (sam@daemoninc.com), 2/11/2007 */

/* compile with: -DFUSE_USE_VERSION=26 -D_FILE_OFFSET_BITS=64 */
/* Can also compile with version 25, but read speed will worse */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#include <signal.h>

#include <fuse.h>
#include <dvdread/dvd_reader.h>

/* Convert a part number (in the vob filename) to an offset within the
 * entire vob.
 */
#define PART_TO_OFFSET(partnum, p_size) ((off_t)(((partnum)-1) * (p_size)))
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))

/* part_size - size of each vob part.  Should probably make this a parameter.
 */
const off_t	part_size	= (1024*1024*1024) - DVD_VIDEO_LB_LEN;

#define MAX_TITLE	99
#define MAX_DOMAIN	(max(DVD_READ_TITLE_VOBS, max(DVD_READ_INFO_BACKUP_FILE, max(DVD_READ_MENU_VOBS, DVD_READ_INFO_FILE))))
#define MIN_DOMAIN	(min(DVD_READ_TITLE_VOBS, min(DVD_READ_INFO_BACKUP_FILE, min(DVD_READ_MENU_VOBS, DVD_READ_INFO_FILE))))

#define IS_VOB(d) ((d) == DVD_READ_TITLE_VOBS || (d) == DVD_READ_MENU_VOBS)
#define IS_IFO(d) (!IS_VOB(d))
#define READ_AHEAD (1024*1024)*2

struct file_info {
	dvd_file_t	*fd;
	off_t		len;
};
struct ext_file_info {
	dvd_read_domain_t	domain;
	int			title;
	struct file_info	*fi;
	off_t			partnum;
	unsigned char		*cache;
	size_t			cache_len;
	off_t			cache_off;
};
struct mount_info {
	dvd_reader_t		*dvd;
	int			num_title;
	struct file_info	file[MAX_DOMAIN+1][MAX_TITLE+1];
        const char              *dvdpath;
	int			num_open;
};

/* Eventually make this "per-mount" instead of global */
static struct mount_info mi;
static volatile int dvd_open;
static time_t file_time = 0x1fedbeef;

/* Translate a dvd video filename into the title#, domain and the part number */
static inline int get_title_domain
    (const char *fname, int *title, dvd_read_domain_t *domain, off_t *partnum)
{
    int vob_is_menu = 0;

    *partnum = 0;

    while (*fname == '/')
	++fname;

    if (strncmp(fname, "VTS_", 4) == 0) {
	if (!isdigit(fname[4]) || !isdigit(fname[5]))
	    return 0;

	*title = atoi(fname+4);
	if (fname[6] != '_')
	    return 0;

	if (fname[7] == '0') {
	    vob_is_menu = 1;
	}
	else if (fname[7] >= '1' && fname[7] <= '9') {
	    *domain = DVD_READ_TITLE_VOBS;
	    *partnum = fname[7] - '0';
	}
	else {
	    return 0;
	}
    }
    else if (strncmp(fname, "VIDEO_TS", 8) == 0) {
	*title = 0;
	vob_is_menu = 1;
    }
    else {
	return 0;
    }

    if (fname[8] != '.')
	return 0;
    if (strcmp(fname+9, "VOB") == 0) {
	if (vob_is_menu)
	    *domain = DVD_READ_MENU_VOBS;
    }
    else if (strcmp(fname+9, "IFO") == 0) {
	*domain = DVD_READ_INFO_FILE;
    }
    else if (strcmp(fname+9, "BUP") == 0) {
	*domain = DVD_READ_INFO_BACKUP_FILE;
    }
    else {
	return 0;
    }
    return 1;
}

static int find_file
    (struct mount_info *mi, const char *name, struct ext_file_info *xfi)
{
    if (strstr(name, "/VIDEO_TS/") == name) {
       name += 9;
    }

    if (!get_title_domain(name, &(xfi->title), &(xfi->domain), &(xfi->partnum)))
	return 0;
    if (xfi->title > mi->num_title)
	return 0;
    xfi->fi = mi->file[xfi->domain] + xfi->title;
    if (!xfi->fi->fd)
	return 0;
    if (xfi->partnum) {
	if (PART_TO_OFFSET(xfi->partnum, part_size) > xfi->fi->len)
	    return 0;
    }

    return 1;
}

/* Open a dvd
 */
static inline int open_dvd(struct mount_info *mi) {
    int title, domain, done;

    if (!(mi->dvd = DVDOpen(mi->dvdpath))) {
	fprintf(stderr, "Could not DVDOpen(%s)\n", mi->dvdpath);
	return -ENOENT;
    }

    /* Every time we re-open dvd, increase the file times in stat by 1-sec
     * so that we invalidate any caching
     */
    file_time++;

    fprintf(stderr, "Dvdopen(%s) worked\n", mi->dvdpath);

    /* Open all the files and determine number of titles. */
    done = 0;
    for (title = 0; !done && title <= MAX_TITLE; ++title) {
	for (domain = MIN_DOMAIN; domain <= MAX_DOMAIN; ++domain) {
	    struct file_info *fi = mi->file[domain]+title;

	    if (title == 0 && domain == DVD_READ_TITLE_VOBS)
		continue;

	    if (!(fi->fd = DVDOpenFile(mi->dvd, title, domain)) &&
		domain == DVD_READ_TITLE_VOBS && title > 0)
	    {
		done = 1;
		break;
	    }
	    fi->len = (off_t)DVDFileSize(fi->fd) * DVD_VIDEO_LB_LEN;
	}
    }
    mi->num_title = title - 1;

    if (mi->num_title < 1) {
	return -ENOENT;
    }

    return 0;
}

/* Close a dvd
 */
static inline void close_dvd(struct mount_info *mi) {
    int title, domain;

    for (title = 0; title <= mi->num_title; ++title) {
	for (domain = MIN_DOMAIN; domain <= MAX_DOMAIN; ++domain) {
	    dvd_file_t *fd = mi->file[domain][title].fd;;
	    if (fd)
		    DVDCloseFile(fd);
	}
    }
    DVDClose(mi->dvd);
    fprintf(stderr, "dvdclose\n");
}


/* Increment the reference count to the dvd.  Open dvd if necessary */
static int inc_refcnt(struct mount_info *mi) {
    if (mi->num_open < 1) {
	signal(SIGALRM, SIG_IGN);
	if (!dvd_open) {
	    int res = open_dvd(mi);
	    if (res < 0)
		return res;
	    dvd_open = 1;
	}
        mi->num_open = 1;
    } else {
	mi->num_open += 1;
    }
    return 0;
}

static void alarm_handler(int x) {
    close_dvd(&mi);
    dvd_open = 0;
}

/* Decrement the reference count to the dvd.  Close dvd if necessary */
static void dec_refcnt(struct mount_info *mi) {
    if (mi->num_open < 1) {
        mi->num_open= 0;
    } else {
        if (mi->num_open == 1) {
	    signal(SIGALRM, &alarm_handler);
	    alarm(5);
	}
        mi->num_open -= 1;
    }
}

static int fs_getattr(const char *name, struct stat *stbuf)
{
    struct ext_file_info xfi;
    int res = 0;

    /* Make sure dvd is open */
    if ((res = inc_refcnt(&mi)) < 0)
	return res;

    /* Init stat struct */
    memset(stbuf, 0, sizeof(struct stat));
    stbuf->st_mtime = stbuf->st_atime = stbuf->st_ctime = file_time;

    if (find_file(&mi, name, &xfi)) {
	stbuf->st_mode = 0100444;
	stbuf->st_nlink = 1;
	stbuf->st_size = xfi.fi->len;
	if (xfi.partnum) {
		stbuf->st_size -= PART_TO_OFFSET(xfi.partnum, part_size);
		if (stbuf->st_size > part_size)
			stbuf->st_size = part_size;
	}
    } else {
	while (*name == '/')
	    ++name;

	if (name[0] == '\0' || (name[0] == '.' && (name[1] == '\0' ||
                 (name[1] == '.' && name[2] == '\0'))))
	{
	    stbuf->st_mode = 040555;
	    stbuf->st_nlink = 2;
	    stbuf->st_size = 512;
	}
	else if (strcmp(name, "VIDEO_TS") == 0) {
	    stbuf->st_mode = 040555;
	    stbuf->st_nlink = 2;
	    stbuf->st_size = 512;
	}
	else {
	    res = -ENOENT;
	}
    }

    /* Release our reference to dvd */
    dec_refcnt(&mi);

    return res;
}


/* add_to_dir: helper for fs_readdir - only add files that exist */
static int add_to_dir (void *buf, fuse_fill_dir_t filler, const char *name) {
    struct ext_file_info xfi;

    if (!find_file(&mi, name, &xfi))
	return 0;
    filler(buf, name, NULL, 0);
    return 1;
}

static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi)
{
    int domain, title, res;

    /* Make sure dvd is open */
    if ((res = inc_refcnt(&mi)) < 0)
	return res;

    /* Standard directory entries */
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    if (strcmp(path, "/") == 0) {
        filler(buf, "VIDEO_TS", NULL, 0);
    } else if (strcmp(path, "/VIDEO_TS") == 0) {
        for (title = 0; title <= mi.num_title; ++title) {
	    for (domain = MIN_DOMAIN; domain <= MAX_DOMAIN; ++domain) {
	        char name[15];

	        if (title > 0) {
		    sprintf(name, "VTS_%02d_0.VOB", title);
	        } else {
		    if (domain == DVD_READ_TITLE_VOBS)
		            continue;
		    strcpy(name, "VIDEO_TS.VOB");
	        }
	        if (domain == DVD_READ_TITLE_VOBS) {
		    int partnum;
		    for (partnum = 1; partnum < 10; ++partnum) {
		        name[7] = partnum + '0';
		        if (!add_to_dir(buf, filler, name))
			    break;
		    }
	        } else {
		    switch(domain) {
		    case DVD_READ_INFO_BACKUP_FILE:
		        strcpy(name+9, "BUP");
		        break;
		    case DVD_READ_INFO_FILE:
		        strcpy(name+9, "IFO");
		        break;
		    }
		    (void) add_to_dir(buf, filler, name);
	        }
	    }
        }
    }

    /* Release our reference to dvd */
    dec_refcnt(&mi);

    return 0;
}

static int fs_open(const char *name, struct fuse_file_info *fi)
{
    struct ext_file_info *xfi = malloc(sizeof(struct ext_file_info));
    int res;

    if (!xfi)
	return -ENOMEM;

    /* Make sure dvd is open */
    if ((res = inc_refcnt(&mi)) < 0)
	return res;

    if (find_file(&mi, name, xfi)) {
	fi->fh = xfi;

	xfi->cache = NULL;
	xfi->cache_len = 0;
	xfi->cache_off = 0;
	
	/* If it's an IFO, read the whole file into the cache */
	if (IS_IFO(xfi->domain) && (xfi->cache = malloc(xfi->fi->len))) {
	    DVDFileSeek(xfi->fi->fd, 0);
	    if (DVDReadBytes(xfi->fi->fd, xfi->cache, xfi->fi->len) == xfi->fi->len) {
#ifdef FIX_IFO
		FIX_IFO(xfi->title, xfi->cache, xfi->fi->len);
#endif
	    } else {
		free(xfi->cache);
		xfi->cache = NULL;
	    }
	}
	return 0;
    }

    /* Failed so release our reference to dvd */
    dec_refcnt(&mi);

    free(xfi);
    return -ENOENT;
}

static int fs_read(const char *path, char *buf, size_t count, off_t offset,
                      struct fuse_file_info *fi)
{
    int res;
    struct ext_file_info *xfi = fi->fh;

    if (!xfi || !xfi->fi->fd)
	return -ENOENT;

    /* fprintf(stderr, "read %s, xfi %d, offset %Ld, count %ld\n", path, xfi, offset, count); */

    if (IS_VOB(xfi->domain)) {
	off_t bk_off;
	size_t bk_cnt;

	if (xfi->domain == DVD_READ_TITLE_VOBS)
	    offset += PART_TO_OFFSET(xfi->partnum, part_size);

	if (offset % DVD_VIDEO_LB_LEN) {
	    return -EIO;
	}

	if (count % DVD_VIDEO_LB_LEN) {
	    return -EIO;
	}
	bk_off = offset / DVD_VIDEO_LB_LEN;
	bk_cnt = count / DVD_VIDEO_LB_LEN;
	
	/* Is this offset/count contained wholly in the cache? */
	if (xfi->cache_off <= offset && offset+count <= xfi->cache_off+xfi->cache_len) {
	    /* Yes, wholly in cache, so use cache only */
	    memcpy(buf, xfi->cache + (offset - xfi->cache_off), count);
	    res = count;
	}
	else {
	    /* No. Read the requested data and then do some read-ahead */
	    res = DVDReadBlocks(xfi->fi->fd, bk_off, bk_cnt, (unsigned char*)buf);
	    if (res > 0) {
		res *= DVD_VIDEO_LB_LEN;

		if (!xfi->cache)
		    xfi->cache = malloc(READ_AHEAD);
		if (xfi->cache && READ_AHEAD >= DVD_VIDEO_LB_LEN) {
		    int res2;

		    xfi->cache_off = offset + res;
		    res2 = DVDReadBlocks(xfi->fi->fd, xfi->cache_off/DVD_VIDEO_LB_LEN, READ_AHEAD/DVD_VIDEO_LB_LEN, xfi->cache);
		    if (res2 < 0)
			res2 = 0;
		    xfi->cache_len = res2 * DVD_VIDEO_LB_LEN;
		}
	    }
	}
    } else {
	if (xfi->cache) {
	    if (offset >= xfi->fi->len || offset < 0) {
		res = 0;
	    } else {
		res = min(count, xfi->fi->len - offset);
		memcpy(buf, xfi->cache + offset, res);
	    }
	} else {
	    res = -EIO;
	}
    }


    /* fprintf(stderr, "read: %d\n", res); */

    return res;
}

static int fs_release(const char *path, struct fuse_file_info *fi) {
    if (fi->fh) {
	struct ext_file_info *xfi = fi->fh;

	if (xfi->cache != NULL) {
	    free(xfi->cache);
	    xfi->cache = NULL;
	}
	free(xfi);
	fi->fh = NULL;

	/* Release this file's reference to the dvd */
	dec_refcnt(&mi);
    }
    return 0;
}

const static struct fuse_operations fs_oper = {
    .getattr	= fs_getattr,
    .readdir	= fs_readdir,
    .open	= fs_open,
    .read	= fs_read,
    .release	= fs_release,
};

int main(int argc, char *argv[])
{
    if (!argv[1]) {
	fprintf(stderr, "Usage: %s device [options] mount-point\n", argv[0]);
	exit(1);
    }
    mi.num_open = 0;
    dvd_open = 0;
    mi.dvdpath = argv[1];
    if (FORCE_SINGLE_THREAD) {
	argv[1] = "-s";
    } else {
	argv++;
	argc--;
    }
    return fuse_main(argc, argv, &fs_oper, NULL);
}
