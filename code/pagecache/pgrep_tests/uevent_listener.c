#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <time.h>
#include <unistd.h>

#define SYSFS_CONF "/etc/sysfs.conf"
#define SYSFS_CONF_NAME "sysfs.conf"
#define TEMP_LOCATION "/var/tmp/"
#define DEFAULT_FNAME "sysfs_tmp_"
#define MAX_BUF_SIZE 1024
#define LEN 256
#define SYSFS_PATH "kernel/mm/page_reclaim/"
#define SYSFS_PATH_GLOBAL "kernel/mm/page_reclaim_global/"
#define PGRECLAIM_SUBSYS "page_reclaim"
#define PGRECLAIM_SUBSYS_GLOBAL "page_reclaim_global"

static int contains(char *zones[], char *buf)
{
	int i, j = 0;
	while (zones[j] != NULL) {
		i = 0;
		while (zones[j][i] != '\0' && buf[i] != '\0') {
			if (zones[j][i] != buf[i])
				break;
			if (zones[j][i] == '=' && buf[i] == '=') {
				return 1;
			}
			i++;
		}
		j++;
	}
	return 0;
}

/* Write contents of conf to temp, replace text in conf with strings in zones. */
static int write_contents(char *zones[], const char *conf, const char *temp)
{
	int j;
	char buf[MAX_BUF_SIZE], *c;
	FILE *cnf, *tmp;

	/* Open files. */
	cnf = fopen(conf, "r");
	if (!cnf) {
		printf("Cannot open file for reading %s.\n", conf);
		return -1;
	}
	tmp = fopen(temp, "w+");
	if (!tmp) {
		printf("Cannot open file for writing %s.\n", temp);
		return -1;
	}

	/* Copy everything from conf that is not in zones. */
	memset(buf, '\0', MAX_BUF_SIZE);
	c = fgets(buf, MAX_BUF_SIZE, cnf);
	while (c != NULL) {
		if(!contains(zones, buf)) {
			fprintf(tmp, "%s", buf);
		}
		memset(buf, '\0', MAX_BUF_SIZE);
		c = fgets(buf, MAX_BUF_SIZE, cnf);
	}

	/* Append zones. */
	j = 0;
	while (zones[j] != NULL) {
		fprintf(tmp, "%s\n", zones[j]);
		j++;
	}

	/* Close files. */
	fclose(cnf);
	fclose(tmp);
	return 0;
}

/* Replace old conf file with new (temp) and delete temp. */
void clean_up(const char *conf, const char *temp, const char *temp_name)
{
	char tmp_conf_location[MAX_BUF_SIZE];

	snprintf(tmp_conf_location, MAX_BUF_SIZE, "%s%s", temp, SYSFS_CONF_NAME);

	if (rename(conf, tmp_conf_location)) {
		printf("Could not move %s to %s.\n", conf, tmp_conf_location);
		return;
	}

	if (rename(temp_name, conf)) {
		printf("Could not move %s to %s.\n", temp_name, conf);
		rename(tmp_conf_location, conf);
		return;
	}

	if (remove(tmp_conf_location)) {
		printf("Could not remove file %s.\n", tmp_conf_location);
		return;
	}
}

static int write_to_config(char *zones[], const char *conf,
			   const char *temp)
{
	int ret;
	char temp_name[MAX_BUF_SIZE];
	time_t t = time(0);

	snprintf(temp_name, MAX_BUF_SIZE, "%s%s%llu", temp, DEFAULT_FNAME,
		 (unsigned long long) t);

	printf("Wrote to conf: %s\n", conf);
	//printf("temp: %s\n", temp_name);
	ret = write_contents(zones, conf, temp_name);
	clean_up(conf, temp, temp_name);
	return ret;
}

static void monitor_kobjevents(const char *conf, const char *temp)
{
	struct udev *udev;
	struct udev_monitor *m;
	struct udev_device *dev;
	int fd, i;
	int seconds = 1;
	char *path = SYSFS_PATH;
	int n = 5; /* At most 5 zones in total*/
	char *attrs[] = 
	{
		"zone_dma",
		"zone_dma32",
		"zone_normal",
		"zone_highmem",
		"zone_movable"
	};
	char *zones[6] = { NULL };

	udev = udev_new();
	if (!udev) {
		printf("Cannot create udev.\n");
		exit(1);
	}

	m = udev_monitor_new_from_netlink(udev, "udev");
	if (!m) {
		printf("Cannot create new udev_monitor.\n");
		exit(1);
	}
	udev_monitor_filter_add_match_subsystem_devtype(m, PGRECLAIM_SUBSYS, NULL);
	udev_monitor_enable_receiving(m);
	fd = udev_monitor_get_fd(m);

	while (1) {
		fd_set fds;
		struct timeval t;
		int ret;

		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		t.tv_sec = 0;
		t.tv_usec = 0;

		ret = select(fd + 1, &fds, NULL, NULL, &t);
		if (ret > 0 && FD_ISSET(fd, &fds)) {
			dev = udev_monitor_receive_device(m);
			printf("\n");
			if (dev) {
				for (i = 0; i < n; i++) {
					if (!zones[i]) {
						zones[i] = (char *) malloc(LEN);
					}
					memset(zones[i], '\0', LEN);
					snprintf(zones[i], LEN, "%s%s%c%s = %s",
						path,
						udev_device_get_sysname(dev),
						'/',
						attrs[i],
						udev_device_get_sysattr_value(dev, attrs[i]));

					printf("%s%s%c%s = %s\n", 
						path,
						udev_device_get_sysname(dev),
						'/',
						attrs[i],
						udev_device_get_sysattr_value(dev, attrs[i]));
				}
				udev_device_unref(dev);
				write_to_config(zones, conf, temp);
			} else {
				printf("No Device from receive_device(). An error occured.\n");
			}
		}
		sleep(seconds);
		printf(".");
		fflush(stdout);
	}
	udev_unref(udev);
}

int main(int argc, char *argv[]) {
	const char *conf = SYSFS_CONF;
	const char *temp = TEMP_LOCATION;

	printf("Usage %s [temp_dir_location] [full sysfs.conf path]\n", argv[0]);
	printf("Default: temp_dir_location:'%s' - sysfs.conf:'%s'\n", temp, conf);
	if (argc == 2) {
		temp = argv[1];
	} else if (argc > 2) {
		temp = argv[1];
		conf = argv[2];
	}

	monitor_kobjevents(conf, temp);
	return 0;
}


