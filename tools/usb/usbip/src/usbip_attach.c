// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 * Copyright (C) 2015-2016 Samsung Electronics
 *               Igor Kotrasinski <i.kotrasinsk@samsung.com>
 *               Krzysztof Opasiak <k.opasiak@samsung.com>
 */

#include "usbip.h"
#include "usbip_network.h"
#include "vhci_driver.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/usbip.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char usbip_attach_usage_string[] =
	"usbip attach <args>\n"
	"    -r, --remote=<host>      The machine with exported USB devices\n"
	"    -b, --busid=<busid>    Busid of the device on <host>\n"
	"    -d, --device=<devid>    Id of the virtual UDC on <host>\n";

void usbip_attach_usage(void)
{
	printf("usage: %s", usbip_attach_usage_string);
}

#define MAX_BUFF 100
static int record_connection(const char *host, const char *port,
			     const char *busid, int rhport)
{
	int fd;
	char path[PATH_MAX+1];
	char buff[MAX_BUFF+1];
	int ret;

	ret = mkdir(VHCI_STATE_PATH, 0700);
	if (ret < 0) {
		/* if VHCI_STATE_PATH exists, then it better be a directory */
		if (errno == EEXIST) {
			struct stat s;

			ret = stat(VHCI_STATE_PATH, &s);
			if (ret < 0)
				return -1;
			if (!(s.st_mode & S_IFDIR))
				return -1;
		} else
			return -1;
	}

	snprintf(path, PATH_MAX, VHCI_STATE_PATH"/port%d", rhport);

	fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU);
	if (fd < 0)
		return -1;

	snprintf(buff, MAX_BUFF, "%s %s %s\n",
			host, port, busid);

	ret = write(fd, buff, strlen(buff));
	if (ret != (ssize_t) strlen(buff)) {
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

static int import_device(int sockfd, struct usbip_usb_device *udev)
{
	int rc;
	int port;
	uint32_t speed = udev->speed;

	rc = usbip_vhci_driver_open();
	if (rc < 0) {
		err("open vhci_driver (is vhci_hcd loaded?)");
		goto err_out;
	}

	do {
		port = usbip_vhci_get_free_port(speed);
		if (port < 0) {
			err("no free port");
			goto err_driver_close;
		}

		dbg("got free port %d", port);

		rc = usbip_vhci_attach_device(port, sockfd, udev->busnum,
					      udev->devnum, udev->speed);
		if (rc < 0 && errno != EBUSY) {
			err("import device");
			goto err_driver_close;
		}
	} while (rc < 0);

	usbip_vhci_driver_close();

	return port;

err_driver_close:
	usbip_vhci_driver_close();
err_out:
	return -1;
}

static int query_import_device(int sockfd, const char *busid)
{
	int rc;
	struct op_import_request request;
	struct op_import_reply   reply;
	uint16_t code = OP_REP_IMPORT;
	int status;

	memset(&request, 0, sizeof(request));
	memset(&reply, 0, sizeof(reply));

	/* send a request */
	rc = usbip_net_send_op_common(sockfd, OP_REQ_IMPORT, 0);
	if (rc < 0) {
		err("send op_common");
		return -1;
	}

	strncpy(request.busid, busid, SYSFS_BUS_ID_SIZE-1);

	PACK_OP_IMPORT_REQUEST(0, &request);

	rc = usbip_net_send(sockfd, (void *) &request, sizeof(request));
	if (rc < 0) {
		err("send op_import_request");
		return -1;
	}

	/* receive a reply */
	rc = usbip_net_recv_op_common(sockfd, &code, &status);
	if (rc < 0) {
		err("Attach Request for %s failed - %s\n",
		    busid, usbip_op_common_status_string(status));
		return -1;
	}

	rc = usbip_net_recv(sockfd, (void *) &reply, sizeof(reply));
	if (rc < 0) {
		err("recv op_import_reply");
		return -1;
	}

	PACK_OP_IMPORT_REPLY(0, &reply);

	/* check the reply */
	if (strncmp(reply.udev.busid, busid, SYSFS_BUS_ID_SIZE)) {
		err("recv different busid %s", reply.udev.busid);
		return -1;
	}

	/* import a device */
	return import_device(sockfd, &reply.udev);
}

static int attach_device(const char *host, const char *port, const char *busid)
{
	int sockfd;
	int rc;
	int rhport;

	sockfd = usbip_net_tcp_connect(host, port);
	if (sockfd < 0) {
		err("tcp connect");
		return -1;
	}

	rhport = query_import_device(sockfd, busid);
	if (rhport < 0)
		return -1;

	close(sockfd);

	rc = record_connection(host, port, busid, rhport);
	if (rc < 0) {
		err("record connection");
		return -1;
	}

	return 0;
}

int usbip_attach(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "remote", required_argument, NULL, 'r' },
		{ "busid",  required_argument, NULL, 'b' },
		{ "device",  required_argument, NULL, 'd' },
		{ NULL, 0,  NULL, 0 }
	};
	char *host = NULL;
	char *busid = NULL;
	int opt;
	int ret = -1;

	for (;;) {
		opt = getopt_long(argc, argv, "d:r:b:", opts, NULL);

		if (opt == -1)
			break;

		switch (opt) {
		case 'r':
			host = optarg;
			break;
		case 'd':
		case 'b':
			busid = optarg;
			break;
		default:
			goto err_out;
		}
	}

	if (!host || !busid)
		goto err_out;

	ret = attach_device(host, usbip_port_string, busid);
	goto out;

err_out:
	usbip_attach_usage();
out:
	return ret;
}

static const char usbip_reattach_usage_string[] =
	"usbip reattach <args>\n"
	"    -p, --port=<port>    " USBIP_VHCI_DRV_NAME
	" port of previously attached device\n";

void usbip_reattach_usage(void)
{
	printf("usage: %s", usbip_reattach_usage_string);
}

static int reattach_port(char *port)
{
	struct usbip_imported_device *idev = NULL;
	unsigned int port_len = strlen(port);
	char busid[SYSFS_BUS_ID_SIZE];
	char host[NI_MAXHOST];
	char serv[NI_MAXSERV];

	uint8_t portnum;
	int ret, i;

	//TODO: should be part of std string to int tools
	for (unsigned int i = 0; i < port_len; i++) {
		if (!isdigit(port[i])) {
			err("invalid port %s", port);
			return -1;
		}
	}

	portnum = atoi(port);

	ret = usbip_vhci_driver_open();
	if (ret < 0) {
		err("open vhci_driver (is vhci_hcd loaded?)");
		return -1;
	}

	for (i = 0; i < vhci_driver->nports; i++) {
		idev = &vhci_driver->idev[i];

		if (idev->port == portnum) {
			if (idev->status == VDEV_ST_NULL ||
			    idev->status == VDEV_ST_NOTASSIGNED)
				break;

			//TODO: add a force flag to trigger a detach first
			/* ret = usbip_vhci_detach_device(portnum); */
			/* if (ret < 0) { */
			/* 	ret = -1; */
			/* 	err("Port %d detach request failed!\n", portnum); */
			/* 	goto call_driver_close; */
			/* } */
			info("Port %d is already attached!\n", idev->port);
			goto call_driver_close;
		}
	}

	if (!idev || i == vhci_driver->nports) {
		ret = -1;
		err("Invalid port %s > maxports %d", port, vhci_driver->nports);
		goto call_driver_close;
	}

	ret = usbip_vhci_read_record(idev->port, host, sizeof(host),
				     serv, sizeof(serv), busid);
	if (ret < 0) {
		err("Failed to get record info port %d", portnum);
		goto call_driver_close;
	}

	ret = attach_device(host, serv, busid);
	if (ret < 0) {
		err("Port %d (re)attach request failed!\n", portnum);
		goto call_driver_close;
	}

	info("Port %d is now (re)attached!\n", portnum);

call_driver_close:
	usbip_vhci_driver_close();

	return ret;
}

int usbip_reattach(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "port", required_argument, NULL, 'p' },
		{ NULL, 0, NULL, 0 }
	};
	int opt;
	int ret = -1;

	for (;;) {
		opt = getopt_long(argc, argv, "p:", opts, NULL);

		if (opt == -1)
			break;

		switch (opt) {
		case 'p':
			ret = reattach_port(optarg);
			goto out;
		default:
			goto err_out;
		}
	}

err_out:
	usbip_reattach_usage();
out:
	return ret;
}
