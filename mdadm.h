/*
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2001-2009 Neil Brown <neilb@suse.de>
 *
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *    Author: Neil Brown
 *    Email: <neilb@suse.de>
 */

#define	_GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include	<unistd.h>
#if !defined(__dietlibc__) && !defined(__KLIBC__)
extern __off64_t lseek64 __P ((int __fd, __off64_t __offset, int __whence));
#else
# if defined(__NO_STAT64) || __WORDSIZE != 32
# define lseek64 lseek
# endif
#endif

#include	<sys/types.h>
#include	<sys/stat.h>
#include	<stdlib.h>
#include	<time.h>
#include	<sys/time.h>
#include	<getopt.h>
#include	<fcntl.h>
#include	<stdio.h>
#include	<errno.h>
#include	<string.h>
#include	<syslog.h>
#ifdef __dietlibc__
#include	<strings.h>
/* dietlibc has deprecated random and srandom!! */
#define random rand
#define srandom srand
#endif


#include	<linux/kdev_t.h>
/*#include	<linux/fs.h> */
#include	<sys/mount.h>
#include	<asm/types.h>
#include	<sys/ioctl.h>
#define	MD_MAJOR 9
#define MdpMinorShift 6

#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12,114,size_t) /* return device size in bytes (u64 *arg) */
#endif

#define DEFAULT_BITMAP_CHUNK 4096
#define DEFAULT_BITMAP_DELAY 5
#define DEFAULT_MAX_WRITE_BEHIND 256

#include	"md_u.h"
#include	"md_p.h"
#include	"bitmap.h"
#include	"msg.h"

#include <endian.h>
/* Redhat don't like to #include <asm/byteorder.h>, and
 * some time include <linux/byteorder/xxx_endian.h> isn't enough,
 * and there is no standard conversion function so... */
/* And dietlibc doesn't think byteswap is ok, so.. */
/*  #include <byteswap.h> */
#define bswap_16(x) (((x) & 0x00ffU) << 8 | \
		     ((x) & 0xff00U) >> 8)
#define bswap_32(x) (((x) & 0x000000ffU) << 24 | \
		     ((x) & 0xff000000U) >> 24 | \
		     ((x) & 0x0000ff00U) << 8  | \
		     ((x) & 0x00ff0000U) >> 8)
#define bswap_64(x) (((x) & 0x00000000000000ffULL) << 56 | \
		     ((x) & 0xff00000000000000ULL) >> 56 | \
		     ((x) & 0x000000000000ff00ULL) << 40 | \
		     ((x) & 0x00ff000000000000ULL) >> 40 | \
		     ((x) & 0x0000000000ff0000ULL) << 24 | \
		     ((x) & 0x0000ff0000000000ULL) >> 24 | \
		     ((x) & 0x00000000ff000000ULL) << 8 | \
		     ((x) & 0x000000ff00000000ULL) >> 8)

#if !defined(__KLIBC__)
#if BYTE_ORDER == LITTLE_ENDIAN
#define	__cpu_to_le16(_x) (_x)
#define __cpu_to_le32(_x) (_x)
#define __cpu_to_le64(_x) (_x)
#define	__le16_to_cpu(_x) (_x)
#define __le32_to_cpu(_x) (_x)
#define __le64_to_cpu(_x) (_x)

#define	__cpu_to_be16(_x) bswap_16(_x)
#define __cpu_to_be32(_x) bswap_32(_x)
#define __cpu_to_be64(_x) bswap_64(_x)
#define	__be16_to_cpu(_x) bswap_16(_x)
#define __be32_to_cpu(_x) bswap_32(_x)
#define __be64_to_cpu(_x) bswap_64(_x)
#elif BYTE_ORDER == BIG_ENDIAN
#define	__cpu_to_le16(_x) bswap_16(_x)
#define __cpu_to_le32(_x) bswap_32(_x)
#define __cpu_to_le64(_x) bswap_64(_x)
#define	__le16_to_cpu(_x) bswap_16(_x)
#define __le32_to_cpu(_x) bswap_32(_x)
#define __le64_to_cpu(_x) bswap_64(_x)

#define	__cpu_to_be16(_x) (_x)
#define __cpu_to_be32(_x) (_x)
#define __cpu_to_be64(_x) (_x)
#define	__be16_to_cpu(_x) (_x)
#define __be32_to_cpu(_x) (_x)
#define __be64_to_cpu(_x) (_x)
#else
#  error "unknown endianness."
#endif
#endif /* __KLIBC__ */


/*
 * min()/max()/clamp() macros that also do
 * strict type-checking.. See the
 * "unnecessary" pointer comparison.
 */
#define min(x, y) ({                            \
	typeof(x) _min1 = (x);                  \
	typeof(y) _min2 = (y);                  \
	(void) (&_min1 == &_min2);              \
	_min1 < _min2 ? _min1 : _min2; })

#define max(x, y) ({                            \
	typeof(x) _max1 = (x);                  \
	typeof(y) _max2 = (y);                  \
	(void) (&_max1 == &_max2);              \
	_max1 > _max2 ? _max1 : _max2; })

/* general information that might be extracted from a superblock */
struct mdinfo {
	mdu_array_info_t	array;
	mdu_disk_info_t		disk;
	__u64			events;
	int			uuid[4];
	char			name[33];
	unsigned long long	data_offset;
	unsigned long long	component_size; /* same as array.size, except in
						 * sectors and up to 64bits.
						 */
	unsigned long long	custom_array_size; /* size for non-default sized
						    * arrays (in sectors)
						    */
	int			reshape_active;
	unsigned long long	reshape_progress;
	union {
		unsigned long long resync_start; /* per-array resync position */
		unsigned long long recovery_start; /* per-device rebuild position */
		#define MaxSector  (~0ULL) /* resync/recovery complete position */
	};
	unsigned long		safe_mode_delay; /* ms delay to mark clean */
	int			new_level, delta_disks, new_layout, new_chunk;
	int			errors;
	int			cache_size; /* size of raid456 stripe cache*/
	int			mismatch_cnt;
	char			text_version[50];
	void 			*update_private; /* for passing metadata-format
						  * specific update data
						  * between successive calls to
						  * update_super()
						  */

	int container_member; /* for assembling external-metatdata arrays
			       * This is to be used internally by metadata
			       * handler only */

	char 		sys_name[20];
	struct mdinfo *devs;
	struct mdinfo *next;

	/* Device info for mdmon: */
	int recovery_fd;
	int state_fd;
	#define DS_FAULTY	1
	#define	DS_INSYNC	2
	#define	DS_WRITE_MOSTLY	4
	#define	DS_SPARE	8
	#define DS_BLOCKED	16
	#define	DS_REMOVE	1024
	#define	DS_UNBLOCK	2048
	int prev_state, curr_state, next_state;

};

struct createinfo {
	int	uid;
	int	gid;
	int	autof;
	int	mode;
	int	symlinks;
	struct supertype *supertype;
};

#define Name "mdadm"

enum mode {
	ASSEMBLE=1,
	BUILD,
	CREATE,
	MANAGE,
	MISC,
	MONITOR,
	GROW,
	INCREMENTAL,
	AUTODETECT,
};

extern char short_options[];
extern char short_bitmap_options[];
extern char short_bitmap_auto_options[];
extern struct option long_options[];
extern char Version[], Usage[], Help[], OptionHelp[],
	Help_create[], Help_build[], Help_assemble[], Help_grow[],
	Help_incr[],
	Help_manage[], Help_misc[], Help_monitor[], Help_config[];

/* for option that don't have short equivilents, we assign arbitrary
 * small numbers.  '1' means an undecorated option, so we start at '2'.
 */
enum special_options {
	AssumeClean = 2,
	BitmapChunk,
	WriteBehind,
	ReAdd,
	NoDegraded,
	Sparc22,
	BackupFile,
	HomeHost,
	AutoHomeHost,
	Symlinks,
	AutoDetect,
	Waitclean,
	DetailPlatform,
};

/* structures read from config file */
/* List of mddevice names and identifiers
 * Identifiers can be:
 *    uuid=128-hex-uuid
 *    super-minor=decimal-minor-number-from-superblock
 *    devices=comma,separated,list,of,device,names,with,wildcards
 *
 * If multiple fields are present, the intersection of all matching
 * devices is considered
 */
#define UnSet (0xfffe)
typedef struct mddev_ident_s {
	char	*devname;

	int	uuid_set;
	int	uuid[4];
	char	name[33];

	unsigned int super_minor;

	char	*devices;	/* comma separated list of device
				 * names with wild cards
				 */
	int	level;
	unsigned int raid_disks;
	unsigned int spare_disks;
	struct supertype *st;
	int	autof;		/* 1 for normal, 2 for partitioned */
	char	*spare_group;
	char	*bitmap_file;
	int	bitmap_fd;

	char	*container;	/* /dev/whatever name of container, or
				 * uuid of container.  You would expect
				 * this to be the 'devname' or UUID
				 * of some other entry.
				 */
	char	*member;	/* subarray within a container */

	struct mddev_ident_s *next;
} *mddev_ident_t;

/* List of device names - wildcards expanded */
typedef struct mddev_dev_s {
	char *devname;
	char disposition;	/* 'a' for add, 'r' for remove, 'f' for fail.
				 * Not set for names read from .config
				 */
	char writemostly;	/* 1 for 'set writemostly', 2 for 'clear writemostly' */
	char re_add;
	char used;		/* set when used */
	struct mdinfo *content;	/* If devname is a container, this might list
				 * the remaining member arrays. */
	struct mddev_dev_s *next;
} *mddev_dev_t;

typedef struct mapping {
	char *name;
	int num;
} mapping_t;


struct mdstat_ent {
	char		*dev;
	int		devnum;
	int		active;
	char		*level;
	char		*pattern; /* U or up, _ for down */
	int		percent; /* -1 if no resync */
	int		resync; /* 1 if resync, 0 if recovery */
	int		devcnt;
	int		raid_disks;
	int		chunk_size;
	char *		metadata_version;
	struct mdstat_ent *next;
};

extern struct mdstat_ent *mdstat_read(int hold, int start);
extern void free_mdstat(struct mdstat_ent *ms);
extern void mdstat_wait(int seconds);
extern void mdstat_wait_fd(int fd, const sigset_t *sigmask);
extern int mddev_busy(int devnum);

struct map_ent {
	struct map_ent *next;
	int	devnum;
	char	metadata[20];
	int	uuid[4];
	int	bad;
	char	*path;
};
extern int map_update(struct map_ent **mpp, int devnum, char *metadata,
		      int uuid[4], char *path);
extern struct map_ent *map_by_uuid(struct map_ent **map, int uuid[4]);
extern struct map_ent *map_by_devnum(struct map_ent **map, int devnum);
extern struct map_ent *map_by_name(struct map_ent **map, char *name);
extern void map_read(struct map_ent **melp);
extern int map_write(struct map_ent *mel);
extern void map_delete(struct map_ent **mapp, int devnum);
extern void map_free(struct map_ent *map);
extern void map_add(struct map_ent **melp,
		    int devnum, char *metadata, int uuid[4], char *path);
extern int map_lock(struct map_ent **melp);
extern void map_unlock(struct map_ent **melp);

/* various details can be requested */
enum sysfs_read_flags {
	GET_LEVEL	= (1 << 0),
	GET_LAYOUT	= (1 << 1),
	GET_COMPONENT	= (1 << 2),
	GET_CHUNK	= (1 << 3),
	GET_CACHE	= (1 << 4),
	GET_MISMATCH	= (1 << 5),
	GET_VERSION	= (1 << 6),
	GET_DISKS	= (1 << 7),
	GET_DEGRADED	= (1 << 8),
	GET_SAFEMODE	= (1 << 9),
	GET_DEVS	= (1 << 10), /* gets role, major, minor */
	GET_OFFSET	= (1 << 11),
	GET_SIZE	= (1 << 12),
	GET_STATE	= (1 << 13),
	GET_ERROR	= (1 << 14),
	SKIP_GONE_DEVS	= (1 << 15),
};

/* If fd >= 0, get the array it is open on,
 * else use devnum. >=0 -> major9. <0.....
 */
extern int sysfs_open(int devnum, char *devname, char *attr);
extern void sysfs_init(struct mdinfo *mdi, int fd, int devnum);
extern void sysfs_free(struct mdinfo *sra);
extern struct mdinfo *sysfs_read(int fd, int devnum, unsigned long options);
extern int sysfs_attr_match(const char *attr, const char *str);
extern int sysfs_match_word(const char *word, char **list);
extern int sysfs_set_str(struct mdinfo *sra, struct mdinfo *dev,
			 char *name, char *val);
extern int sysfs_set_num(struct mdinfo *sra, struct mdinfo *dev,
			 char *name, unsigned long long val);
extern int sysfs_uevent(struct mdinfo *sra, char *event);
extern int sysfs_get_fd(struct mdinfo *sra, struct mdinfo *dev,
			char *name);
extern int sysfs_fd_get_ll(int fd, unsigned long long *val);
extern int sysfs_get_ll(struct mdinfo *sra, struct mdinfo *dev,
			char *name, unsigned long long *val);
extern int sysfs_fd_get_str(int fd, char *val, int size);
extern int sysfs_get_str(struct mdinfo *sra, struct mdinfo *dev,
			 char *name, char *val, int size);
extern int sysfs_set_safemode(struct mdinfo *sra, unsigned long ms);
extern int sysfs_set_array(struct mdinfo *info, int vers);
extern int sysfs_add_disk(struct mdinfo *sra, struct mdinfo *sd, int resume);
extern int sysfs_disk_to_scsi_id(int fd, __u32 *id);
extern int sysfs_unique_holder(int devnum, long rdev);
extern int load_sys(char *path, char *buf);


extern int save_stripes(int *source, unsigned long long *offsets,
			int raid_disks, int chunk_size, int level, int layout,
			int nwrites, int *dest,
			unsigned long long start, unsigned long long length,
			char *buf);
extern int restore_stripes(int *dest, unsigned long long *offsets,
			   int raid_disks, int chunk_size, int level, int layout,
			   int source, unsigned long long read_offset,
			   unsigned long long start, unsigned long long length);

#ifndef Sendmail
#define Sendmail "/usr/lib/sendmail -t"
#endif

#define SYSLOG_FACILITY LOG_DAEMON

extern char *map_num(mapping_t *map, int num);
extern int map_name(mapping_t *map, char *name);
extern mapping_t r5layout[], r6layout[], pers[], modes[], faultylayout[];

extern char *map_dev(int major, int minor, int create);

struct active_array;
struct metadata_update;

/* A superswitch provides entry point the a metadata handler.
 *
 * The super_switch primarily operates on some "metadata" that
 * is accessed via the 'supertype'.
 * This metadata has one of three possible sources.
 * 1/ It is read from a single device.  In this case it may not completely
 *    describe the array or arrays as some information might be on other
 *    devices.
 * 2/ It is read from all devices in a container.  In this case all
 *    information is present.
 * 3/ It is created by ->init_super / ->add_to_super.  In this case it will
 *    be complete once enough ->add_to_super calls have completed.
 *
 * When creating an array inside a container, the metadata will be
 * formed by a combination of 2 and 3.  The metadata or the array is read,
 * then new information is added.
 *
 * The metadata must sometimes have a concept of a 'current' array
 * and a 'current' device.
 * The 'current' array is set by init_super to be the newly created array,
 * or is set by super_by_fd when it finds it is looking at an array inside
 * a container.
 *
 * The 'current' device is either the device that the metadata was read from
 * in case 1, or the last device added by add_to_super in case 3.
 * Case 2 does not identify a 'current' device.
 */
extern struct superswitch {

	/* Used to report details of metadata read from a component
	 * device. ->load_super has been called.
	 */
	void (*examine_super)(struct supertype *st, char *homehost);
	void (*brief_examine_super)(struct supertype *st, int verbose);
	void (*brief_examine_subarrays)(struct supertype *st, int verbose);
	void (*export_examine_super)(struct supertype *st);

	/* Used to report details of an active array.
	 * ->load_super was possibly given a 'component' string.
	 */
	void (*detail_super)(struct supertype *st, char *homehost);
	void (*brief_detail_super)(struct supertype *st);
	void (*export_detail_super)(struct supertype *st);

	/* Optional: platform hardware / firmware details */
	int (*detail_platform)(int verbose, int enumerate_only);

	/* Used:
	 *   to get uuid to storing in bitmap metadata
	 *   and 'reshape' backup-data metadata
	 *   To see if a device is being re-added to an array it was part of.
	 */
	void (*uuid_from_super)(struct supertype *st, int uuid[4]);

	/* Extract generic details from metadata.  This could be details about
	 * the container, or about an individual array within the container.
	 * The determination is made either by:
	 *   load_super being given a 'component' string.
	 *   validate_geometry determining what to create.
	 * The info includes both array information and device information.
	 * The particular device should be:
	 *   The last device added by add_to_super
	 *   The device the metadata was loaded from by load_super
	 */
	void (*getinfo_super)(struct supertype *st, struct mdinfo *info);

	/* Check if the given metadata is flagged as belonging to "this"
	 * host.  0 for 'no', 1 for 'yes', -1 for "Don't record homehost"
	 */
	int (*match_home)(struct supertype *st, char *homehost);

	/* Make one of several generic modifications to metadata
	 * prior to assembly (or other times).
	 *   sparc2.2  - first bug in early 0.90 metadata
	 *   super-minor - change name of 0.90 metadata
	 *   summaries - 'correct' any redundant data
	 *   resync - mark array as dirty to trigger a resync.
	 *   uuid - set new uuid - only 0.90 or 1.x
	 *   name - change the name of the array (where supported)
	 *   homehost - change which host this array is tied to.
	 *   devicesize - If metadata is at start of device, change recorded
	 *               device size to match actual device size
	 *   byteorder - swap bytes for 0.90 metadata
	 *
	 *   force-one  - mark that device as uptodate, not old or failed.
	 *   force-array - mark array as clean if it would not otherwise
	 *               assemble
	 *   assemble   - not sure how this is different from force-one...
	 *   linear-grow-new - add a new device to a linear array, but don't
	 *                   change the size: so superblock still matches
	 *   linear-grow-update - now change the size of the array.
	 */
	int (*update_super)(struct supertype *st, struct mdinfo *info,
			    char *update,
			    char *devname, int verbose,
			    int uuid_set, char *homehost);

	/* Create new metadata for new array as described.  This could
	 * be a new container, or an array in a pre-existing container.
	 * Also used to zero metadata prior to writing it to invalidate old
	 * metadata.
	 */
	int (*init_super)(struct supertype *st, mdu_array_info_t *info,
			  unsigned long long size, char *name,
			  char *homehost, int *uuid);

	/* update the metadata to include new device, either at create or
	 * when hot-adding a spare.
	 */
	int (*add_to_super)(struct supertype *st, mdu_disk_info_t *dinfo,
			     int fd, char *devname);

	/* Write metadata to one device when fixing problems or adding
	 * a new device.
	 */
	int (*store_super)(struct supertype *st, int fd);

	/*  Write all metadata for this array.
	 */
	int (*write_init_super)(struct supertype *st);
	int (*compare_super)(struct supertype *st, struct supertype *tst);
	int (*load_super)(struct supertype *st, int fd, char *devname);
	struct supertype * (*match_metadata_desc)(char *arg);
	__u64 (*avail_size)(struct supertype *st, __u64 size);
	int (*add_internal_bitmap)(struct supertype *st, int *chunkp,
				   int delay, int write_behind,
				   unsigned long long size, int may_change, int major);
	void (*locate_bitmap)(struct supertype *st, int fd);
	int (*write_bitmap)(struct supertype *st, int fd);
	void (*free_super)(struct supertype *st);

	/* validate_geometry is called with an st returned by
	 * match_metadata_desc.
	 * It should check that the geometry described in compatible with
	 * the metadata type.  It will be called repeatedly as devices
	 * added to validate changing size and new devices.  If there are
	 * inter-device dependencies, it should record sufficient details
	 * so these can be validated.
	 * Both 'size' and '*freesize' are in sectors.  chunk is bytes.
	 */
	int (*validate_geometry)(struct supertype *st, int level, int layout,
				 int raiddisks,
				 int chunk, unsigned long long size,
				 char *subdev, unsigned long long *freesize,
				 int verbose);

	struct mdinfo *(*container_content)(struct supertype *st);
	/* Allow a metadata handler to override mdadm's default layouts */
	int (*default_layout)(int level); /* optional */

/* for mdmon */
	int (*open_new)(struct supertype *c, struct active_array *a,
			char *inst);

	/* Tell the metadata handler the current state of the array.
	 * This covers whether it is known to be consistent (no pending writes)
	 * and how far along a resync is known to have progressed
	 * (in a->resync_start).
	 * resync status is really irrelevant if the array is not consistent,
	 * but some metadata (DDF!) have a place to record the distinction.
	 * If 'consistent' is '2', then the array can mark it dirty if a 
	 * resync/recovery/whatever is required, or leave it clean if not.
	 * Return value is 0 dirty (not consistent) and 1 if clean.
	 * it is only really important if consistent is passed in as '2'.
	 */
	int (*set_array_state)(struct active_array *a, int consistent);

	/* When the state of a device might have changed, we call set_disk to
	 * tell the metadata what the current state is.
	 * Typically this happens on spare->in_sync and (spare|in_sync)->faulty
	 * transitions.
	 * set_disk might be called when the state of the particular disk has
	 * not in fact changed.
	 */
	void (*set_disk)(struct active_array *a, int n, int state);
	void (*sync_metadata)(struct supertype *st);
	void (*process_update)(struct supertype *st,
			       struct metadata_update *update);
	void (*prepare_update)(struct supertype *st,
			       struct metadata_update *update);

	/* activate_spare will check if the array is degraded and, if it
	 * is, try to find some spare space in the container.
	 * On success, it add appropriate updates (For process_update) to
	 * to the 'updates' list and returns a list of 'mdinfo' identifying
	 * the device, or devices as there might be multiple missing
	 * devices and multiple spares available.
	 */
	struct mdinfo *(*activate_spare)(struct active_array *a,
					 struct metadata_update **updates);

	int swapuuid; /* true if uuid is bigending rather than hostendian */
	int external;
	const char *name; /* canonical metadata name */
} super0, super1, super_ddf, *superlist[];

extern struct superswitch super_imsm;

struct metadata_update {
	int	len;
	char	*buf;
	void	*space; /* allocated space that monitor will use */
	struct metadata_update *next;
};

/* A supertype holds a particular collection of metadata.
 * It identifies the metadata type by the superswitch, and the particular
 * sub-version of that metadata type.
 * metadata read in or created is stored in 'sb' and 'info'.
 * There are also fields used by mdmon to track containers.
 *
 * A supertype may refer to:
 *   Just an array, possibly in a container
 *   A container, not identifying any particular array
 *   Info read from just one device, not yet fully describing the array/container.
 *
 *
 * A supertype is created by:
 *   super_by_fd
 *   guess_super
 *   dup_super
 */
struct supertype {
	struct superswitch *ss;
	int minor_version;
	int max_devs;
	int container_dev;    /* devnum of container */
	char subarray[32];	/* name of array inside container */
	void *sb;
	void *info;
	int loaded_container;	/* Set if load_super found a container,
				 * not just one device */

	struct metadata_update *updates;
	struct metadata_update **update_tail;

	/* extra stuff used by mdmon */
	struct active_array *arrays;
	int sock; /* listen to external programs */
	int devnum;
	char *devname; /* e.g. md0.  This appears in metadata_verison:
			*  external:/md0/12
			*/
	int devcnt;

	struct mdinfo *devs;

};

extern struct supertype *super_by_fd(int fd);
extern struct supertype *guess_super(int fd);
extern struct supertype *dup_super(struct supertype *st);
extern int get_dev_size(int fd, char *dname, unsigned long long *sizep);
extern void get_one_disk(int mdfd, mdu_array_info_t *ainf,
			 mdu_disk_info_t *disk);
void wait_for(char *dev, int fd);

#if __GNUC__ < 3
struct stat64;
#endif

#define HAVE_NFTW  we assume
#define HAVE_FTW

#ifdef __UCLIBC__
# include <features.h>
# ifndef __UCLIBC_HAS_LFS__
#  define lseek64 lseek
# endif
# ifndef  __UCLIBC_HAS_FTW__
#  undef HAVE_FTW
#  undef HAVE_NFTW
# endif
#endif

#ifdef __dietlibc__
# undef HAVE_NFTW
#endif

#if defined(__KLIBC__)
# undef HAVE_NFTW
# undef HAVE_FTW
#endif

#ifndef HAVE_NFTW
# define FTW_PHYS 1
# ifndef HAVE_FTW
  struct FTW {};
# endif
#endif

#ifdef HAVE_FTW
# include <ftw.h>
#endif

extern int add_dev(const char *name, const struct stat *stb, int flag, struct FTW *s);


extern int Manage_ro(char *devname, int fd, int readonly);
extern int Manage_runstop(char *devname, int fd, int runstop, int quiet);
extern int Manage_resize(char *devname, int fd, long long size, int raid_disks);
extern int Manage_subdevs(char *devname, int fd,
			  mddev_dev_t devlist, int verbose);
extern int autodetect(void);
extern int Grow_Add_device(char *devname, int fd, char *newdev);
extern int Grow_addbitmap(char *devname, int fd, char *file, int chunk, int delay, int write_behind, int force);
extern int Grow_reshape(char *devname, int fd, int quiet, char *backup_file,
			long long size,
			int level, char *layout_str, int chunksize, int raid_disks);
extern int Grow_restart(struct supertype *st, struct mdinfo *info,
			int *fdlist, int cnt, char *backup_file, int verbose);
extern int Grow_continue(int mdfd, struct supertype *st,
			 struct mdinfo *info, char *backup_file);

extern int Assemble(struct supertype *st, char *mddev,
		    mddev_ident_t ident,
		    mddev_dev_t devlist, char *backup_file,
		    int readonly, int runstop,
		    char *update, char *homehost, int require_homehost,
		    int verbose, int force);

extern int Build(char *mddev, int chunk, int level, int layout,
		 int raiddisks, mddev_dev_t devlist, int assume_clean,
		 char *bitmap_file, int bitmap_chunk, int write_behind,
		 int delay, int verbose, int autof, unsigned long long size);


extern int Create(struct supertype *st, char *mddev,
		  int chunk, int level, int layout, unsigned long long size, int raiddisks, int sparedisks,
		  char *name, char *homehost, int *uuid,
		  int subdevs, mddev_dev_t devlist,
		  int runstop, int verbose, int force, int assume_clean,
		  char *bitmap_file, int bitmap_chunk, int write_behind, int delay, int autof);

extern int Detail(char *dev, int brief, int export, int test, char *homehost);
extern int Detail_Platform(struct superswitch *ss, int scan, int verbose);
extern int Query(char *dev);
extern int Examine(mddev_dev_t devlist, int brief, int export, int scan,
		   int SparcAdjust, struct supertype *forcest, char *homehost);
extern int Monitor(mddev_dev_t devlist,
		   char *mailaddr, char *alert_cmd,
		   int period, int daemonise, int scan, int oneshot,
		   int dosyslog, int test, char *pidfile, int increments);

extern int Kill(char *dev, struct supertype *st, int force, int quiet, int noexcl);
extern int Wait(char *dev);
extern int WaitClean(char *dev, int sock, int verbose);

extern int Incremental(char *devname, int verbose, int runstop,
		       struct supertype *st, char *homehost, int require_homehost,
		       int autof);
extern int Incremental_container(struct supertype *st, char *devname,
				 int verbose, int runstop, int autof,
				 int trustworthy);
extern void RebuildMap(void);
extern int IncrementalScan(int verbose);

extern int CreateBitmap(char *filename, int force, char uuid[16],
			unsigned long chunksize, unsigned long daemon_sleep,
			unsigned long write_behind,
			unsigned long long array_size,
			int major);
extern int ExamineBitmap(char *filename, int brief, struct supertype *st);
extern int bitmap_update_uuid(int fd, int *uuid, int swap);
extern unsigned long bitmap_sectors(struct bitmap_super_s *bsb);

extern int md_get_version(int fd);
extern int get_linux_version(void);
extern long long parse_size(char *size);
extern int parse_uuid(char *str, int uuid[4]);
extern int parse_layout_10(char *layout);
extern int parse_layout_faulty(char *layout);
extern int check_ext2(int fd, char *name);
extern int check_reiser(int fd, char *name);
extern int check_raid(int fd, char *name);

extern int get_mdp_major(void);
extern int dev_open(char *dev, int flags);
extern int open_dev(int devnum);
extern int open_dev_excl(int devnum);
extern int is_standard(char *dev, int *nump);
extern int same_dev(char *one, char *two);

extern int parse_auto(char *str, char *msg, int config);
extern mddev_ident_t conf_get_ident(char *dev);
extern mddev_dev_t conf_get_devs(void);
extern int conf_test_dev(char *devname);
extern int conf_test_metadata(const char *version);
extern struct createinfo *conf_get_create_info(void);
extern void set_conffile(char *file);
extern char *conf_get_mailaddr(void);
extern char *conf_get_mailfrom(void);
extern char *conf_get_program(void);
extern char *conf_get_homehost(int *require_homehostp);
extern char *conf_line(FILE *file);
extern char *conf_word(FILE *file, int allow_key);
extern int conf_name_is_free(char *name);
extern int devname_matches(char *name, char *match);
extern struct mddev_ident_s *conf_match(struct mdinfo *info, struct supertype *st);

extern void free_line(char *line);
extern int match_oneof(char *devices, char *devname);
extern void uuid_from_super(int uuid[4], mdp_super_t *super);
extern const int uuid_match_any[4];
extern int same_uuid(int a[4], int b[4], int swapuuid);
extern void copy_uuid(void *a, int b[4], int swapuuid);
extern char *__fname_from_uuid(int id[4], int swap, char *buf, char sep);
extern char *fname_from_uuid(struct supertype *st,
			     struct mdinfo *info, char *buf, char sep);
extern unsigned long calc_csum(void *super, int bytes);
extern int enough(int level, int raid_disks, int layout, int clean,
		   char *avail, int avail_disks);
extern int ask(char *mesg);
extern unsigned long long get_component_size(int fd);
extern void remove_partitions(int fd);
extern unsigned long long calc_array_size(int level, int raid_disks, int layout,
				   int chunksize, unsigned long long devsize);
extern int flush_metadata_updates(struct supertype *st);
extern void append_metadata_update(struct supertype *st, void *buf, int len);
extern int assemble_container_content(struct supertype *st, int mdfd,
				      struct mdinfo *content, int runstop,
				      char *chosen_name, int verbose);

extern int add_disk(int mdfd, struct supertype *st,
		    struct mdinfo *sra, struct mdinfo *info);
extern int set_array_info(int mdfd, struct supertype *st, struct mdinfo *info);
unsigned long long min_recovery_start(struct mdinfo *array);

extern char *human_size(long long bytes);
extern char *human_size_brief(long long bytes);
extern void print_r10_layout(int layout);

#define NoMdDev (1<<23)
extern int find_free_devnum(int use_partitions);

extern void put_md_name(char *name);
extern char *get_md_name(int dev);

extern char DefaultConfFile[];

extern int create_mddev(char *dev, char *name, int autof, int trustworthy,
			char *chosen);
/* values for 'trustworthy' */
#define	LOCAL	1
#define	FOREIGN	2
#define	METADATA 3
extern int open_mddev(char *dev, int report_errors);
extern int open_container(int fd);

extern int mdmon_running(int devnum);
extern int signal_mdmon(int devnum);
extern int check_env(char *name);
extern __u32 random32(void);
extern int start_mdmon(int devnum);

extern char *devnum2devname(int num);
extern int devname2devnum(char *name);
extern int stat2devnum(struct stat *st);
extern int fd2devnum(int fd);

static inline int dev2major(int d)
{
	if (d >= 0)
		return MD_MAJOR;
	else
		return get_mdp_major();
}

static inline int dev2minor(int d)
{
	if (d >= 0)
		return d;
	return (-1-d) << MdpMinorShift;
}

static inline int ROUND_UP(int a, int base)
{
	return ((a+base-1)/base)*base;
}

static inline int is_subarray(char *vers)
{
	/* The version string for a 'subarray' (an array in a container)
	 * is 
	 *    /containername/componentname    for normal read-write arrays
	 *    -containername/componentname    for read-only arrays.
	 * containername is e.g. md0, md_d1
	 * componentname is dependant on the metadata. e.g. '1' 'S1' ...
	 */
	return (*vers == '/' || *vers == '-');
}

#ifdef DEBUG
#define dprintf(fmt, arg...) \
	fprintf(stderr, fmt, ##arg)
#else
#define dprintf(fmt, arg...) \
        ({ if (0) fprintf(stderr, fmt, ##arg); 0; })
#endif
#include <assert.h>
#include <stdarg.h>
static inline int xasprintf(char **strp, const char *fmt, ...) {
	va_list ap;
	int ret;
	va_start(ap, fmt);
	ret = vasprintf(strp, fmt, ap);
	va_end(ap);
	assert(ret >= 0);
	return ret;
}

#define	LEVEL_MULTIPATH		(-4)
#define	LEVEL_LINEAR		(-1)
#define	LEVEL_FAULTY		(-5)

/* kernel module doesn't know about these */
#define LEVEL_CONTAINER		(-100)
#define	LEVEL_UNSUPPORTED	(-200)


/* faulty stuff */

#define	WriteTransient	0
#define	ReadTransient	1
#define	WritePersistent	2
#define	ReadPersistent	3
#define	WriteAll	4 /* doesn't go to device */
#define	ReadFixable	5
#define	Modes	6

#define	ClearErrors	31
#define	ClearFaults	30

#define AllPersist	100 /* internal use only */
#define	NoPersist	101

#define	ModeMask	0x1f
#define	ModeShift	5


#ifdef __TINYC__
#undef minor
#undef major
#undef makedev
#define minor(x) ((x)&0xff)
#define major(x) (((x)>>8)&0xff)
#define makedev(M,m) (((M)<<8) | (m))
#endif

/* for raid4/5/6 */
#define ALGORITHM_LEFT_ASYMMETRIC	0
#define ALGORITHM_RIGHT_ASYMMETRIC	1
#define ALGORITHM_LEFT_SYMMETRIC	2
#define ALGORITHM_RIGHT_SYMMETRIC	3

/* Define non-rotating (raid4) algorithms.  These allow
 * conversion of raid4 to raid5.
 */
#define ALGORITHM_PARITY_0		4 /* P or P,Q are initial devices */
#define ALGORITHM_PARITY_N		5 /* P or P,Q are final devices. */

/* DDF RAID6 layouts differ from md/raid6 layouts in two ways.
 * Firstly, the exact positioning of the parity block is slightly
 * different between the 'LEFT_*' modes of md and the "_N_*" modes
 * of DDF.
 * Secondly, or order of datablocks over which the Q syndrome is computed
 * is different.
 * Consequently we have different layouts for DDF/raid6 than md/raid6.
 * These layouts are from the DDFv1.2 spec.
 * Interestingly DDFv1.2-Errata-A does not specify N_CONTINUE but
 * leaves RLQ=3 as 'Vendor Specific'
 */

#define ALGORITHM_ROTATING_ZERO_RESTART	8 /* DDF PRL=6 RLQ=1 */
#define ALGORITHM_ROTATING_N_RESTART	9 /* DDF PRL=6 RLQ=2 */
#define ALGORITHM_ROTATING_N_CONTINUE	10 /*DDF PRL=6 RLQ=3 */


/* For every RAID5 algorithm we define a RAID6 algorithm
 * with exactly the same layout for data and parity, and
 * with the Q block always on the last device (N-1).
 * This allows trivial conversion from RAID5 to RAID6
 */
#define ALGORITHM_LEFT_ASYMMETRIC_6	16
#define ALGORITHM_RIGHT_ASYMMETRIC_6	17
#define ALGORITHM_LEFT_SYMMETRIC_6	18
#define ALGORITHM_RIGHT_SYMMETRIC_6	19
#define ALGORITHM_PARITY_0_6		20
#define ALGORITHM_PARITY_N_6		ALGORITHM_PARITY_N

