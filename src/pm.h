/* The port manager's logic: the catalog, what's installed on the card, checking GitHub for
 * releases and installing updates (only files from a release are written; the player's
 * game data, saves and settings never; replaced files and the port's backup folders go to
 * Backups/PortManager/ first). Everything stays in Onion's Ports list. */
#ifndef PM_H
#define PM_H

typedef struct {
	char *id, *name, *dir, *layout, *repo, *asset, *category, *get;
	char *rom, *sc_gdf, *sc_run;
	char **skip, **backup, **required, **required_any;
	int nskip, nbackup, nrequired, nrequired_any;

	/* on the card (pm_scan) */
	int installed;
	char version[64];
	char missing[512]; /* game files the player still has to add, "" when all there */

	/* from the last pm_check */
	int checked;      /* the latest release is known */
	char latest[64];
	int update;       /* installed, version known and older than latest */
} Port;

extern Port *ports;
extern int nports;
extern char pm_error[512]; /* why the last pm_check/pm_update failed */

int pm_init(void);   /* paths, log, the catalog on the card, pm_scan; -1 if no catalog */
void pm_scan(void);  /* installed, version, missing for every port */
int pm_lock(void);   /* one run at a time; -1 if another is running */
void pm_unlock(void);

/* network: refreshes the catalog from GitHub, checks the connection, fetches the latest
 * release of every installed port's repo; with `only`, just that port's repo and no catalog
 * refresh (the Port stays valid); 0, or -1 with pm_error */
int pm_check(const Port *only);
/* installs the latest release over an installed port; 0, or -1 with pm_error */
int pm_update(Port *p);

/* a port's details and how to add its game files */
void pm_info(const Port *p, char *buf, int size);

#endif
