/*==========================================================================
Description
  Wcnss_filter provides mux/demux of Bluetooth and ANT data sent over the same
  UART channel

# Copyright (c) 2013-2014, 2016 Qualcomm Technologies, Inc.
# All Rights Reserved.
# Confidential and Proprietary - Qualcomm Technologies, Inc.

===========================================================================*/

/*
 * wcnss_filter code has three processing threads,

 * Main/Transport_read_thread: This thread would open the UART channel and select
 * for the data/events coming over UART port. Whenever there is a data available
 * from the controller, this thread would read the protocol byte and route the data
 * to available client handles (either Bluetooth or ANT client sockets)

 * Bluetooth client thread: This thread create server socket (UNIX domain socket)
 * for Bluetooth client and wait for the incoming connection from Bluetooth stack.
 * Upon valid client connection, the thread would start selecting on Bluetooth
 * client socket,so that commands/data coming from Bluetooth stack/Host would be
 * read and passed to UART transport. These threads have the logic of closing of
 * their ends as part of client closure and start waiting for the connection for
 * next client connection

 * ANT client thread: This thread create server socket (UNIX domain socket) for ANT
 * client and wait for the incoming connection from ANT stack. Upon valid client
 * connection, the thread would start selecting on ANT client socket, so that
 * commands/data coming from ANT stack/Host would be read and passed to UART
 * transport.  These threads have the logic of closing of their ends as part of
 * client closure and start waiting for the connection for next client connection.

 * Writes to UART port is guarded with a mutex so that writes from different
 * clients would be synchronized as required.
**/

#include <cutils/log.h>
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/un.h>
#include <cutils/properties.h>
#include "private/android_filesystem_config.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif

#define LOG_TAG "WCNSS_FILTER"

#define BT_SOCK "bt_sock"
#define ANT_SOCK "ant_sock"

#define BT_HS_UART_DEVICE "/dev/ttySAC0"

#define BT_SSR_TRIGGERED 0xee

#define BT_CMD_PACKET_TYPE 0x01
#define BT_ACL_PACKET_TYPE 0x02
#define BT_SCO_PACKET_TYPE 0x03
#define BT_EVT_PACKET_TYPE 0x04
#define ANT_CTL_PACKET_TYPE 0x0c
#define ANT_DATA_PACKET_TYPE 0x0e

#define MAX_BT_HDR_SIZE 4

#define BT_ACL_HDR_SIZE 4
#define BT_SCO_HDR_SIZE 3
#define BT_EVT_HDR_SIZE 2
#define BT_CMD_HDR_SIZE 3

#define BT_ACL_HDR_LEN_OFFSET 2
#define BT_SCO_HDR_LEN_OFFSET 2
#define BT_EVT_HDR_LEN_OFFSET 1
#define BT_CMD_HDR_LEN_OFFSET 2

#define ANT_CMD_HDR_SIZE      2
#define ANT_HDR_OFFSET_LEN    1

#ifndef BLUETOOTH_UID
#define BLUETOOTH_UID 1002
#endif
#ifndef SYSTEM_UID
#define SYSTEM_UID 1000
#endif

#ifndef ROOT_UID
#define ROOT_UID 0
#endif

#ifndef AID_USER
#define AID_USER 100000
#endif

#ifndef AID_APP
#define AID_APP 10000
#endif

#define HOST_TO_SOC 0
#define SOC_TO_HOST 1

pthread_mutex_t signal_mutex;

int remote_bt_fd;
int remote_ant_fd;

int fd_transport;

static pthread_t bt_mon_thread;
static pthread_t ant_mon_thread;

int copy_bt_data_to_channel(int src_fd, int dest_fd, unsigned char protocol_byte,int dir);
int copy_ant_host_data_to_soc(int src_fd, int dest_fd, unsigned char protocol_byte);

static void handle_cleanup();
static int do_write(int fd, unsigned char *buf, int len);

unsigned char reset_cmpl[] = {0x04, 0x0e, 0x04, 0x01,0x03, 0x0c, 0x00};

static int extract_uid(int uuid)
{
    int userid;
    int appid;

    appid = userid =  uuid % AID_USER;
    if (userid > BLUETOOTH_UID)
    {
        appid = userid % AID_APP;
    }
    ALOGD("%s appid = %d",__func__,appid);
    return appid;
}

static int establish_remote_socket(char *name)
{
    int fd = -1;
    struct sockaddr_un client_address;
    socklen_t clen;
    int sock_id, ret;
    struct ucred creds;
    int c_uid;
    ALOGV("%s(%s) Entry  ", __func__, name);

    sock_id = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (sock_id < 0) {
        ALOGE("%s: server Socket creation failure", __func__);
        return fd;
    }

    ALOGV("convert name to android abstract name:%s %d", name, sock_id);
    if (socket_local_server_bind(sock_id,
        name, ANDROID_SOCKET_NAMESPACE_ABSTRACT) >= 0) {
        if (listen(sock_id, 5) == 0) {
            ALOGV("listen to local socket:%s, fd:%d", name, sock_id);
        } else {
            ALOGE("listen to local socket:failed");
            close(sock_id);
            return fd;
        }
    } else {
        close(sock_id);
        ALOGE("%s: server bind failed for socket : %s", __func__, name);
        return fd;
    }

    clen = sizeof(client_address);
    ALOGV("%s: before accept_server_socket", name);
    fd = accept(sock_id, (struct sockaddr *)&client_address, &clen);
    if (fd > 0) {
        ALOGV("%s accepted fd:%d for server fd:%d", name, fd, sock_id);
        close(sock_id);

        memset(&creds, 0, sizeof(creds));
        socklen_t szCreds = sizeof(creds);
        ret = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &creds, &szCreds);
        if (ret < 0) {
            ALOGE("%s: error getting remote socket creds: %d\n", __func__, ret);
            close(fd);
            return -1;
        }
        c_uid = creds.uid;
        if (c_uid > BLUETOOTH_UID)
            c_uid = extract_uid(creds.uid);
        if (c_uid != BLUETOOTH_UID && c_uid != SYSTEM_UID
                && c_uid != ROOT_UID) {
            ALOGE("%s: client doesn't have required credentials", __func__);
            ALOGE("<%s req> client uid: %d", name, creds.uid);
            close(fd);
            return -1;
        }

        ALOGV("%s: Remote socket credentials: %d\n", __func__, creds.uid);
        return fd;
    } else {
        ALOGE("BTC accept failed fd:%d sock d:%d error %s", fd, sock_id, strerror(errno));
        close(sock_id);
        return fd;
    }

    close(sock_id);
    return fd;
}

#ifdef DEBUG_MIMIC_CMD_TOUT
/* This mimics the CMD TOUT by dropping the
 * "set Local name" command at filter
 * user can repro the cmd tout by setting the name
 * couple of times from UI
 */
int command_is_change_lname(unsigned char* buf, int len) {
    int val =1, i;
    unsigned char lname[] = {0x01, 0x13, 0x0c};
    ALOGE("%s:", __func__);
    for (i=0; i<3; i++) {
       if(buf[i] != lname[i]) val = 0;
    }
    return val;
}
#endif //DEBUG_MIMIC_CMD_TOUT

#ifdef IGNORE_HCI_RESET
/**
  * this is to ignore HCI_RESET coming from stack.
  * as performing HCI_RESET when ANT is on is not recomended.
  * HCI_RESET only executed once from libbt-vendor as part of
  * chip and NVM initialization
  */
int command_is_reset(unsigned char* buf, int len) {
    int val = 1, i;
    unsigned char reset[] = {0x01, 0x03, 0x0c, 0x00};
    ALOGE("%s:", __func__);
    for (i=0; i<len; i++) {
       if(buf[i] != reset[i]) val = 0;
    }
    return val;
}
#endif//IGNORE_HCI_RESET

int handle_command_writes(int fd) {
    ALOGV("%s: ", __func__);
    unsigned char first_byte;
    int retval;

    ALOGV("%s: BT/ANT--Host-To-SoC: Reading 1st byte to determine the "
        "sub-system(BT/ANT) and HCI PKT type(CMD/DATA)", __func__);

    retval = read (fd, &first_byte, 1);
    if (retval < 0) {
        ALOGE("%s:read returns err: %d\n", __func__,retval);
        return -1;
    }

    if (retval == 0) {
        ALOGE("%s: This indicates the close of other end", __func__);
        return -99;
    }

    ALOGV("%s: Protocol_byte: %x", __func__, first_byte);
    switch(first_byte) {
        case ANT_CTL_PACKET_TYPE:
        case ANT_DATA_PACKET_TYPE:
            ALOGV("%s: Ant data", __func__);
            retval = copy_ant_host_data_to_soc(fd, fd_transport, first_byte);
            break;
        case BT_EVT_PACKET_TYPE:
        case BT_ACL_PACKET_TYPE:
        case BT_CMD_PACKET_TYPE:
            ALOGV("%s: BT data", __func__);
            retval = copy_bt_data_to_channel(fd, fd_transport, first_byte, HOST_TO_SOC);
            break;
        case BT_SSR_TRIGGERED:
            ALOGV("It is SSR triggered from command tout");
            break;
        default:
            ALOGE("%s: Unexpected data format!!",__func__);
            retval = -1;
    }

    ALOGV("%s: retval %d", __func__, 0);
    return 0;
}

static int bt_thread() {
    fd_set client_fds;
    int retval, n;

    ALOGV("%s: Entry ", __func__);
    do {
        remote_bt_fd = establish_remote_socket(BT_SOCK);

        if (remote_bt_fd < 0) {
            ALOGE("%s: invalid remote socket", __func__);
            return -1;
        }

        FD_ZERO(&client_fds);
        FD_SET(remote_bt_fd, &client_fds);

        do {
            ALOGV("%s: Back in BT select loop", __func__);
            n = select(remote_bt_fd+1, &client_fds, NULL, NULL, NULL);
            if(n < 0){
                ALOGE("Select: failed: %s", strerror(errno));
                break;
            }
            ALOGV("%s: select came out\n", __func__);
            if (FD_ISSET(remote_bt_fd, &client_fds)) {
                retval = handle_command_writes(remote_bt_fd);
                ALOGV("%s: handle_command_writes . %d", __func__, retval);
                if(retval < 0) {
                    if (retval == -99) {
                        ALOGV("%s:End of wait loop", __func__);
                    }
                    ALOGV("%s: handle_command_writes returns: %d: ", __func__, retval);
                    break;
                }
            }
        } while(1);

        ALOGI("%s: Bluetooth turned off", __func__);
        close(remote_bt_fd);
        remote_bt_fd = 0;
        handle_cleanup();
    } while(1);

    pthread_exit(NULL);
    return 0;
}

static int ant_thread() {
    fd_set client_fds;
    int retval, n;

    ALOGV("%s: Entry ", __func__);
    do {
        remote_ant_fd = establish_remote_socket(ANT_SOCK);
        if (remote_ant_fd < 0) {
            ALOGE("%s: invalid remote socket", __func__);
            return -1;
        }

        FD_ZERO(&client_fds);
        FD_SET(remote_ant_fd, &client_fds);
        do {
            ALOGV("%s: Back in ANT select loop", __func__);
            n = select(remote_ant_fd+1, &client_fds, &client_fds, NULL, NULL);
            if(n < 0){
                ALOGE("Select: failed: %s", strerror(errno));
                break;
            }
            ALOGV("%s: Step 2-ANT-HTS: ANT CMD/DATA available for processing...\n", __func__);
            if (FD_ISSET(remote_ant_fd, &client_fds)) {
                retval = handle_command_writes(remote_ant_fd);
                if(retval < 0) {
                   if (retval == -99) {
                       ALOGV("%s:End of wait loop", __func__);
                   }
                   ALOGV("%s: handle_command_writes returns: %d: ", __func__, retval);
                   break;
                }
            }
            ALOGV("%s: moving back to Select loop", __func__);
        } while(1);

        ALOGI("%s: ANT turned off", __func__);
        close(remote_ant_fd);
        remote_ant_fd = 0;
        handle_cleanup();
    } while(1);

    pthread_exit(NULL);
    return 0;
}

static int init_transport() {
    struct termios   term;
    uint32_t baud = B3000000;
    uint8_t stop_bits = 0;

    ALOGV("%s: Entry ", __func__);

    if ((fd_transport = open(BT_HS_UART_DEVICE, O_RDWR)) == -1) {
        ALOGE("%s: Unable to open %s: %d (%s)", __func__, BT_HS_UART_DEVICE,
           fd_transport, strerror(errno));
        return -1;
    }

    if (tcflush(fd_transport, TCIOFLUSH) < 0) {
        ALOGE("issue while tcflush %s", BT_HS_UART_DEVICE);
        close(fd_transport);
        return -1;
    }

    if (tcgetattr(fd_transport, &term) < 0) {
        ALOGE("issue while tcgetattr %s", BT_HS_UART_DEVICE);
        close(fd_transport);
        return -1;
    }

    cfmakeraw(&term);
    /* Set RTS/CTS HW Flow Control*/
    term.c_cflag |= (CRTSCTS | stop_bits);

    if (tcsetattr(fd_transport, TCSANOW, &term) < 0) {
       ALOGE("issue while tcsetattr %s", BT_HS_UART_DEVICE);
       close(fd_transport);
       return -1;
    }

    if (tcflush(fd_transport, TCIOFLUSH) < 0) {
        ALOGE("after enabling flags issue while tcflush %s", BT_HS_UART_DEVICE);
        close(fd_transport);
        return -1;
    }

    if (tcsetattr(fd_transport, TCSANOW, &term) < 0) {
       ALOGE("issue while tcsetattr %s", BT_HS_UART_DEVICE);
       close(fd_transport);
       return -1;
    }

    if (tcflush(fd_transport, TCIOFLUSH) < 0) {
        ALOGE("after enabling flags issue while tcflush %s", BT_HS_UART_DEVICE);
        close(fd_transport);
        return -1;
    }

    /* set input/output baudrate */
    cfsetospeed(&term, baud);
    cfsetispeed(&term, baud);
    tcsetattr(fd_transport, TCSANOW, &term);
    ALOGV("%s returns fd: %d", __func__, fd_transport);
    return fd_transport;
}

static int do_write(int fd, unsigned char *buf,int len)
{
    int ret = 0;
    int write_offset = 0;
    int write_len = len;
    do {
        ret = write(fd, buf+write_offset, write_len);
        if (ret < 0)
        {
            ALOGE("%s: write failed ret = %d err = %s",__func__,ret,strerror(errno));
            return -1;

        } else if (ret == 0) {
            ALOGE("%s: Write returned 0, err = %s, Written bytes: %d, expected: %d",
                        __func__, strerror(errno), (len-write_len), len);
            return (len-write_len);

        } else {
            if (ret < write_len)
            {
               ALOGD("%s, Write pending,do write ret = %d err = %s",__func__,ret,
                       strerror(errno));
               write_len = write_len - ret;
               write_offset = ret;
            } else if (ret > write_len) {
               ALOGE("%s: FATAL wrote more than expected: written bytes: %d expected: %d",
                      __func__, write_len, ret);
               break;
            } else {
               ALOGV("Write successful");
               break;
            }
        }
    } while(1);
    return len;
}

int do_read(int fd, unsigned char* buf, size_t len) {
   int bytes_left, bytes_read = 0, read_offset;

   if (!len) {
      ALOGV("%s: read returned with len 0.", __func__);
      return 0;
   }

   bytes_left = len;
   read_offset = 0;

   do {
       bytes_read = read(fd, buf+read_offset, bytes_left);
       if (bytes_read < 0) {
           ALOGE("%s: Read error: %d (%s)", __func__, bytes_left, strerror(errno));
           return -1;
       } else if (bytes_read == 0) {
            ALOGE("%s: read returned 0, err = %s, read bytes: %d, expected: %d",
                              __func__, strerror(errno),(unsigned int) (len-bytes_left),(unsigned int) len);
            return (len-bytes_left);
       }
       else {
           if (bytes_read < bytes_left) {
              ALOGV("Still there are %d bytes to read", bytes_left-bytes_read);
              bytes_left = bytes_left-bytes_read;
              read_offset = read_offset+bytes_read;
           } else {
              ALOGV("%s: done with read",__func__);
              break;
           }
       }
   }while(1);
   return len;
}

int copy_bt_data_to_channel(int src_fd, int dest_fd, unsigned char protocol_byte,int direction) {
    unsigned char len;
    unsigned short acl_len;
    unsigned char* buf;
    unsigned char hdr[MAX_BT_HDR_SIZE];
    bool no_valid_client = false;
    int retval, i;

    ALOGV("%s: Entry.. proto byte : %d\n", __func__, protocol_byte);
    if (dest_fd == 0) {
        ALOGE("%s: No valid BT client connection", __func__);
        /*Discard the packet and keep the read loop alive*/
        no_valid_client = true;
    }

    switch(protocol_byte) {
       case BT_ACL_PACKET_TYPE:
           retval = do_read(src_fd, hdr, BT_ACL_HDR_SIZE);
           if (retval < 0) {
               ALOGE("%s:error in reading hdr: %d",__func__, retval);
               return -1;
           }

           /*ACL data len in two bytes in length*/
           acl_len = *((unsigned short*)&hdr[BT_ACL_HDR_LEN_OFFSET]);
           ALOGV("acl_len: %d\n", acl_len);
           buf = (unsigned char*)calloc(acl_len+BT_ACL_HDR_SIZE+1, sizeof(char));
           if (buf == NULL) {
               ALOGE("%s:alloc error", __func__);
               return -2;
           }
           buf[0] = protocol_byte;
           memcpy(buf+1, hdr, BT_ACL_HDR_SIZE);

           retval = do_read(src_fd, buf+1+BT_ACL_HDR_SIZE, acl_len);
           if (retval < 0) {
               ALOGE("%s:error in reading buf: %d", __func__, retval);
               retval = -1;
               free(buf);
               return retval;
           }
           acl_len = acl_len+BT_ACL_HDR_SIZE+1;
           break;

       case BT_SCO_PACKET_TYPE:
       case BT_CMD_PACKET_TYPE:
           retval = do_read(src_fd, hdr, BT_SCO_HDR_SIZE);
           if (retval < 0) {
               ALOGE("%s:error in reading hdr: %d",__func__, retval);
               return -1;
           }

           len = hdr[BT_SCO_HDR_LEN_OFFSET];
           ALOGV("len: %d\n", len);
           buf = (unsigned char*)calloc(len+BT_SCO_HDR_SIZE+1, sizeof(char));
           if (buf == NULL) {
               ALOGE("%s:alloc error", __func__);
               return -2;
           }

           buf[0] = protocol_byte;
           memcpy(buf+1, hdr, BT_SCO_HDR_SIZE);
           retval = do_read(src_fd, buf+BT_SCO_HDR_SIZE+1, len);
           if (retval < 0) {
               ALOGE("%s:error in reading buf: %d", __func__, retval);
               retval = -1;
               free(buf);
               return retval;
           }
           acl_len = len+BT_SCO_HDR_SIZE+1;
           break;

       case BT_EVT_PACKET_TYPE:
           retval = do_read(src_fd, hdr, BT_EVT_HDR_SIZE);
           if (retval < 0) {
               ALOGE("%s:error in reading hdr: %d", __func__, retval);
               return -1;
           }

           len = hdr[BT_EVT_HDR_LEN_OFFSET];
           ALOGV("len: %d\n", len);
           buf = (unsigned char*)calloc(len+BT_EVT_HDR_SIZE+1, sizeof(char));
           if (buf == NULL) {
               ALOGE("%s:alloc error" ,__func__);
               return -2;
           }

           buf[0] = protocol_byte;
           memcpy(buf+1, hdr, BT_EVT_HDR_SIZE);
           retval = do_read(src_fd, buf+BT_EVT_HDR_SIZE+1, len);
           if (retval < 0) {
               ALOGE("%s:error in reading buf: %d", __func__, retval);
               retval = -1;
               free(buf);
               return retval;
           }
           acl_len = len+BT_EVT_HDR_SIZE+1;
           break;
       default:
          ALOGE("%s: packet type error", __func__);
          return -3;
     }
     if (no_valid_client || remote_bt_fd == 0) {
          /*Discard the packet and keep the read loop alive*/
          ALOGE("BT is turned off in b/w, keep back in loop");
          free(buf);
          return 0;
     }
#ifdef DEBUG_MIMIC_CMD_TOUT
     if ( command_is_change_lname(buf, acl_len) ) {
         ALOGE("Drop the change local name cmd");
         free(buf);
         return 0;
     }
#endif //DEBUG_MIMIC_CMD_TOUT
#ifdef IGNORE_HCI_RESET
      if (acl_len == 4 && command_is_reset(buf, acl_len))
      {
         ALOGV("It is an HCI_RESET Command ");
         //Dont write it controller rather mimmc success event
         retval = write (src_fd, reset_cmpl, 7);
         if (retval < 0) {
              ALOGE("%s: error while writing hci_reset_cmp", __func__);
         }
         free(buf);
         return retval;
      }
#endif//IGNORE_HCI_RESET

     pthread_mutex_lock(&signal_mutex);
     retval = do_write(dest_fd, buf, acl_len);
     pthread_mutex_unlock(&signal_mutex);
     if (retval < 0) {
         ALOGE("%s:error in writing buf: %d: %s", __func__, retval, strerror(errno));
         if (errno == EPIPE || errno == EBADF) {
             ALOGV("%s: BT has closed of the other end", __func__);
             /*return 0, so that read loop continues*/
             retval = 0;
         } else {
             retval = -1;
         }
         free(buf);
         return retval;
     }

     ALOGV("Direction(%d): bytes: %d : bytes_written: %d", direction, acl_len, retval);
     for (i =0; i<acl_len; i++) {
         ALOGV("%x-", buf[i]);
     }
     ALOGV("*done");

     ALOGV("%s: copied bt data/cmd (of len %d) succesfully\n", __func__, acl_len);

     free(buf);
     return retval;
}


int copy_ant_host_data_to_soc(int src_fd, int dest_fd, unsigned char protocol_byte) {
    unsigned char hdr[ANT_CMD_HDR_SIZE];
    int len;
    unsigned char *ant_pl;
    int i, retval;
    bool no_valid_client = false;

    ALOGV("%s: entry", __func__);
    if (dest_fd == 0) {
        ALOGE("%s: No valid ANT client connection", __func__);
        /*Discard the packet and keep the read loop alive*/
        no_valid_client = true;
    }

    /*Read protocol byte*/
    retval = do_read(src_fd, &(hdr[ANT_HDR_OFFSET_LEN]), ANT_HDR_OFFSET_LEN);
    if (retval < 0) {
        ALOGE("%s: read length returns err: %d\n", __func__,retval);
        return -1;
    }
    len =  hdr[ANT_HDR_OFFSET_LEN];
    hdr[0] = protocol_byte;
    ALOGV("%s: size of the data is: %d\n", __func__, len);
    ant_pl = (unsigned char*)malloc(len+ANT_CMD_HDR_SIZE);
    if (ant_pl == NULL) {
        ALOGE("Malloc error\n");
        return -1;
    }

    retval = do_read(src_fd, ant_pl+ANT_CMD_HDR_SIZE, (int)len);
    if (retval < 0) {
        ALOGE("read returns err: %d\n", retval);
        retval = -1;
        free(ant_pl);
        return retval;
    }

    memcpy(ant_pl, hdr, ANT_CMD_HDR_SIZE);

    pthread_mutex_lock(&signal_mutex);
    retval = do_write(dest_fd, ant_pl, len+ANT_CMD_HDR_SIZE);
    pthread_mutex_unlock(&signal_mutex);
    if (retval < 0) {
        ALOGE("write returns err: file_desc: %d %d(%s)\n", dest_fd, retval,strerror(errno));
        retval = -1;
        free(ant_pl);
        return retval;
    }

    ALOGV("ANT host bytes sent*");
    for (i =0; i<retval; i++) {
         ALOGV("%x-", ant_pl[i]);
    }

    free(ant_pl);
    return retval;
}

int copy_ant_data_to_channel(int src_fd, int dest_fd, unsigned char protocol_byte)
{
    unsigned char len;
    unsigned char* ant_pl;
    int retval = 0,i;

    ALOGV("%s: Entry ", __func__);

    int ret = read (src_fd, &len, 1);
    if (ret < 0) {
        ALOGE("%s: read length returns err: %d\n", __func__,ret);
        return -1;
    }

    ALOGV("%s: size of the data is: %d\n", __func__, len);
    ant_pl = (unsigned char*)malloc(len+2);

    if (ant_pl == NULL) {
        ALOGE("Malloc error\n");
        return -1;
    }

    ret = do_read(src_fd, ant_pl+2, (int)len);
    if (ret < 0) {
        ALOGE("read returns err: %d\n", ret);
        retval = -1;
        free(ant_pl);
        return retval;
    }

    if (remote_ant_fd == 0) {
        /*Discard the packet and keep the read loop alive*/
        free(ant_pl);
        return 0;
    }

    if (ret < len)
        ALOGV("%s: expected %d bytes, recieved only %d", __func__, len, ret);

    ant_pl[0] = protocol_byte;
    ant_pl[1] = len;
    pthread_mutex_lock(&signal_mutex);
    ret = do_write(dest_fd, ant_pl+1, ret+1);
    pthread_mutex_unlock(&signal_mutex);

    if (ret < 0) {
        ALOGE("write returns err: file_desc: %d %d(%s)\n", dest_fd, ret,strerror(errno));
        if (errno == EPIPE || errno == EBADF) {
            ALOGV("%s: ANT has closed of the other end", __func__);
             /*return 0, so that read loop continues*/
             retval = 0;
        } else {
             retval = -1;
        }
        free(ant_pl);
        return retval;
    }

    ALOGV("ANT event bytes sent*");
    for (i =0; i<ret; i++) {
         ALOGV("%x-", ant_pl[i]);
    }
    ALOGV("*done");

    ALOGV("%s: copied ant data(of len %d) succesfully\n", __func__,len);

    free(ant_pl);
    ALOGV("%s: retval: %d\n", __func__,retval);
    return retval;
}

int handle_soc_events(int fd_transport) {
    unsigned char first_byte;
    int retval;
    ALOGV("%s: Entry ", __func__);

    retval = read (fd_transport, &first_byte, 1);
    if (retval < 0) {
        ALOGE("%s:read returns err: %d\n", __func__,retval);
        return -1;
    }

    ALOGV("%s: protocol_byte: %x", __func__, first_byte);

    switch(first_byte) {
        case ANT_CTL_PACKET_TYPE:
        case ANT_DATA_PACKET_TYPE:
            ALOGV("%s: Ant data", __func__);
            retval = copy_ant_data_to_channel(fd_transport, remote_ant_fd, first_byte);
            ALOGV("%s: copy_ant_data_to_channel returns %d", __func__, retval);
            break;
        case BT_EVT_PACKET_TYPE:
        case BT_ACL_PACKET_TYPE:
            ALOGV("%s: BT data", __func__);
            retval = copy_bt_data_to_channel(fd_transport, remote_bt_fd, first_byte,SOC_TO_HOST);
            break;
        default:
            ALOGE("%s: Unexpected data format!!:%x - Ignore the Packet ",__func__,first_byte);
            //retval = -1;
            tcflush(fd_transport, TCIFLUSH);
            retval = 0 ;
    }

    ALOGV("%s: Exit %d", __func__, retval);
    return retval;
}

static int start_reader_thread() {
    fd_set input;
    int n = 0, retval;
    ALOGV("%s: Entry ", __func__);

    if ((fd_transport = init_transport()) == -1) {
        ALOGE("unable to initialize transport %s", BT_HS_UART_DEVICE);
        return -1;
    }

    /*Indicate that, server is ready to accept*/
    property_set("vendor.wc_transport.hci_filter_status", "1");

    FD_ZERO(&input);
    FD_SET(fd_transport, &input);

    do {
        ALOGV("%s: Selecting on transport for events", __func__);
        n = select (fd_transport+1, &input, NULL, NULL, NULL);

        if(n < 0){
            ALOGE("Select failed: %s", strerror(errno));
            retval = -1;
            break;
        }

        ALOGV("%s: Select comes out\n", __func__);
        if (fd_transport < 0) {
            ALOGE("%s: fd_transport is already deinit, exit loop",__func__);
            retval = -1;
            break;
        }

        if (FD_ISSET(fd_transport, &input)) {
             retval = handle_soc_events(fd_transport);
             if(retval < 0) {
                 ALOGE("%s: handle_soc_events returns: %d: ", __func__, retval);
                 retval = -1;
                 break;
            }
        }
    } while(1);

    close(fd_transport);
    fd_transport = 0;
    ALOGV("%s: Exit %d", __func__, retval);
    return retval;
}

int cleanup_thread(pthread_t thread) {
    int status = 0;
    ALOGV("%s: Entry", __func__);

    if((status = pthread_join(thread, NULL)) != 0) {
        ALOGE("Error joining thread %d, error = %d (%s)",
      (int)thread, status, strerror(status));
    }
    ALOGV("%s: End", __func__);
    return status;
}

int main() {
    int ret;
    ALOGV("%s: Entry", __func__);
    signal(SIGPIPE, SIG_IGN);

    pthread_mutex_init(&signal_mutex, NULL);
    if (pthread_create(&bt_mon_thread, NULL, (void *)bt_thread, NULL) != 0) {
        perror("pthread_create for bt_monitor");
        ret = -1;
        goto bt_thread_fail;
    }

    if (pthread_create(&ant_mon_thread, NULL, (void *)ant_thread, NULL) != 0) {
        perror("pthread_create for ant_monitor");
        ret = -1;
        goto ant_thread_fail;
    }

    /*Main thread monitors on UART data/events*/
    ret = start_reader_thread();

    if (ret < 0) {
        ALOGE("%s: start_reader_thread returns: %d", __func__, ret);
    }

ant_thread_fail:
    cleanup_thread(ant_mon_thread);
bt_thread_fail:
    cleanup_thread(bt_mon_thread);

    pthread_mutex_destroy(&signal_mutex);

    ALOGV("%s: Exit: %d", __func__, ret);
    property_set("vendor.wc_transport.hci_filter_status", "0");
    property_set("vendor.wc_transport.start_hci", "false");
    return ret;
}

static void handle_cleanup()
{
    char ref_count[PROPERTY_VALUE_MAX];
    char cleanup[PROPERTY_VALUE_MAX];
    int ref_val,clean;

    ALOGE("wcnss_filter client is terminated");
    property_get("vendor.wc_transport.clean_up", cleanup, "0");
    clean = atoi(cleanup);
    ALOGE("clean Value =  %d",clean);
    property_get("vendor.wc_transport.ref_count", ref_count, "0");
    ref_val = atoi(ref_count);
    if(clean == 0) {
      if(ref_val > 0)
      {
         ref_val--;
         snprintf(ref_count, 3, "%d", ref_val);
         property_set("vendor.wc_transport.ref_count", ref_count);
      }
    }
    if (!remote_bt_fd && !remote_ant_fd) {
        char value[PROPERTY_VALUE_MAX] = {'\0'};

        ALOGD("%s",__func__);

        property_get("vendor.wc_transport.hci_filter_status", value, "0");
        if (!strcmp(value, "0")) {
            ALOGI("%s: wcnss_filter has been stopped already", __func__);
            return;
        } else
            property_set("vendor.wc_transport.hci_filter_status", "0");

        //property_set("vendor.wc_transport.soc_initialized", "0");
        property_set("vendor.wc_transport.start_hci", "false");
        ALOGE("Done with this Life!!!");
        exit(0);
    }
}
