/* The port manager's logic: the catalog, what's installed on the card, checking GitHub for
 * releases and installing updates (only files from a release are written; the player's
 * game data, saves and settings never; replaced files and the port's backup folders go to
 * Backups/PortManager/ first). Everything stays in Onion's Ports list. */
#ifndef PM_H
#define PM_H

typedef struct {
	char *id, *name, *dir, *layout, *repo, *asset, *category, *get;
	char *status; /* playable, experimental, prerelease, or "" */
	char *app;    /* app-to-ports: the zip's App/<app> when not dir's last part, or "" */
	char *rom, *rom_name, *sc_gdf, *sc_run;
	char **skip, **backup, **required, **required_any, **built;
	int nskip, nbackup, nrequired, nrequired_any, nbuilt;

	/* on the card (pm_scan) */
	int installed;
	char version[64];
	char missing[512]; /* game files the player still has to add, "" when all there */

	/* from the last pm_check */
	int checked;      /* the latest release is known */
	char latest[64];
	int update;       /* installed, version known and older than latest */
	long asset_size;  /* size in bytes of the asset, 0 if unknown */
	char asset_name[128]; /* filename of the asset in release */
} Port;

extern Port *ports;
extern int nports;
extern char pm_error[512]; /* why the last pm_check/pm_update failed */

int pm_init(void);   /* paths, log, the catalog on the card, pm_scan; -1 if it is broken */
extern int pm_no_catalog; /* none on the card yet: the first check downloads it */
void pm_scan(void);  /* installed, version, missing for every port */
int pm_lock(void);   /* one run at a time; -1 if another is running */
void pm_unlock(void);

/* network: refreshes the catalog from GitHub, checks the connection, fetches the latest
 * release of every installed port's repo; with `only`, just that port's repo and no catalog
 * refresh (the Port stays valid); 0, or -1 with pm_error */
int pm_check(const Port *only);
/* installs the latest release over an installed port; 0, or -1 with pm_error */
int pm_update(Port *p);
/* removes an installed port; if keep_data is set, ROMs, game data and saves in its folder
 * are kept; 0, or -1 with pm_error */
int pm_uninstall(Port *p, int keep_data);
extern int pm_folder_kept; /* after pm_uninstall: the game folder is still there (data kept) */

/* release notes / changelog (from GitHub release body) for a checked port */
const char *pm_release_notes(const Port *p);

/* a port's details and how to add its game files */
void pm_info(const Port *p, char *buf, int size);
void pm_details(const Port *p, char *buf, int size);

/* backup management */
typedef struct {
	char path[800];
	char display[192];
	char tag[64];
	char date[64];
	long size;
	int is_archive;
} BackupItem;

int pm_list_backups(const Port *p, BackupItem **items, int *n);
void pm_free_backups(BackupItem *items, int n);
int pm_restore_backup(Port *p, const BackupItem *b);

/* remote releases */
typedef struct {
	char tag[64];
	char name[128];
	char asset_name[128];
	long asset_size;
	char download_url[512];
	char api_url[512];
	char digest[128];
	char *body;
} RemoteRelease;

int pm_list_remote_releases(const Port *p, RemoteRelease **items, int *n);
void pm_free_remote_releases(RemoteRelease *items, int n);
int pm_install_release(Port *p, const RemoteRelease *rel);

/* self-update (Port Manager updating itself) */
#define PM_VERSION "1.2.4"
#define PM_REPO "ksielick/portmanager-miyoo"

extern int pm_self_update_available;
extern char pm_self_latest_version[64];
extern long pm_self_asset_size;

const char *pm_version(void);
const char *pm_here(void);
/* network: Port Manager's own latest release only (done before the ports are checked);
 * sets pm_self_update_available; 0, or -1 with pm_error */
int pm_check_self(void);
int pm_self_update(void);

#endif

