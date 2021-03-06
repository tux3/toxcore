/* Messenger.c
 *
 * An implementation of a simple text chat only messenger on the tox network core.
 *
 *  Copyright (C) 2013 Tox project All Rights Reserved.
 *
 *  This file is part of Tox.
 *
 *  Tox is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Tox is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef DEBUG
#include <assert.h>
#endif

#include "logger.h"
#include "Messenger.h"
#include "network.h"
#include "util.h"
#include "save.h"

static void set_friend_status(Tox *tox, int32_t friendnumber, uint8_t status);
static void set_device_status(Messenger *m, int32_t friendnumber, int32_t device_id, uint8_t status);
static int write_cryptpacket_id(const Tox *tox, int32_t friendnumber, uint8_t packet_id, const uint8_t *data,
                                uint32_t length, uint8_t congestion_control);

// friend_not_valid determines if the friendnumber passed is valid in the Messenger object
static uint8_t friend_not_valid(const Messenger *m, int32_t friendnumber)
{
    if ((unsigned int)friendnumber < m->numfriends) {
        if (m->friendlist[friendnumber].status != 0) {
            return 0;
        }
    }

    return 1;
}

/* Set the size of the friend list to numfriends.
 *
 *  return -1 if realloc fails.
 */
int realloc_friendlist(Messenger *m, uint32_t num)
{
    if (num == 0) {
        free(m->friendlist);
        m->friendlist = NULL;
        return 0;
    }

    Friend *newfriendlist = realloc(m->friendlist, num * sizeof(Friend));

    if (newfriendlist == NULL)
        return -1;

    m->friendlist = newfriendlist;
    return 0;
}

/* Set the size of the device list to num.
 *
 *  return -1 if realloc fails.
 */
static int realloc_dev_list(Messenger *m, uint32_t fr_num, uint32_t num)
{
    if (num == 0) {
        free(m->friendlist[fr_num].dev_list);
        m->friendlist[fr_num].dev_list = NULL;
        return 0;
    }

    F_Device *newlist = realloc(m->friendlist[fr_num].dev_list, num * sizeof(F_Device));

    if (newlist == NULL) {
        return -1;
    }

    m->friendlist[fr_num].dev_list = newlist;
    return 0;
}

/*  return the friend id associated to that public key.
 *  return -1 if no such friend.
 */
int32_t getfriend_id(const Messenger *m, const uint8_t *real_pk)
{
    uint32_t i, device;

    for (i = 0; i < m->numfriends; ++i) {
        if (m->friendlist[i].status > 0) {
            for(device = 0; device < m->friendlist[i].dev_count; ++device) {
                if (id_equal(real_pk, m->friendlist[i].dev_list[device].real_pk)) {
                    return i;
                }
            }
        }
    }

    return -1;
}

int32_t getfriend_devid(const Messenger *m, const uint8_t *real_pk)
{
    uint32_t i, device;

    for (i = 0; i < m->numfriends; ++i) {
        if (m->friendlist[i].status > 0) {
            for(device = 0; device < m->friendlist[i].dev_count; ++device) {
                if (id_equal(real_pk, m->friendlist[i].dev_list[device].real_pk)) {
                    return device;
                }
            }
        }
    }

    return -1;
}

/* Copies the public key associated to that friend id into real_pk buffer.
 * Make sure that real_pk is of size crypto_box_PUBLICKEYBYTES.
 *
 *  return 0 if success.
 *  return -1 if failure.
 */
int get_real_pk(const Tox *tox, int32_t friendnumber, uint8_t *real_pk)
{
    if (friend_not_valid(tox->m, friendnumber)) {
        return -1;
    }

    /* TODO: we should return an array here? Or maybe replace/entend this fxn */
    memcpy(real_pk, tox->m->friendlist[friendnumber].dev_list[0].real_pk, crypto_box_PUBLICKEYBYTES);
    return 0;
}

/*  return friend connection id on success.
 *  return -1 if failure.
 */
int getfriendcon_id(const Tox *tox, int32_t friendnumber)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    /* TODO: we should return an array here? Or maybe replace/entend this fxn */
    return tox->m->friendlist[friendnumber].dev_list[0].friendcon_id;
}

/*
 *  return a uint16_t that represents the checksum of address of length len.
 */
static uint16_t address_checksum(const uint8_t *address, uint32_t len)
{
    uint8_t checksum[2] = {0};
    uint16_t check;
    uint32_t i;

    for (i = 0; i < len; ++i)
        checksum[i % 2] ^= address[i];

    memcpy(&check, checksum, sizeof(check));
    return check;
}

/* Format: [real_pk (32 bytes)][nospam number (4 bytes)][checksum (2 bytes)]
 *
 *  return FRIEND_ADDRESS_SIZE byte address to give to others.
 */
void getaddress(const Tox *tox, uint8_t *address)
{
    id_copy(address, tox->net_crypto->self_public_key);
    uint32_t nospam = get_nospam(tox->net_crypto);
    memcpy(address + crypto_box_PUBLICKEYBYTES, &nospam, sizeof(nospam));
    uint16_t checksum = address_checksum(address, FRIEND_ADDRESS_SIZE - sizeof(checksum));
    memcpy(address + crypto_box_PUBLICKEYBYTES + sizeof(nospam), &checksum, sizeof(checksum));
}

static int send_online_packet(Tox *tox, int32_t friendnumber, int32_t device_num)
{
    if (friend_not_valid(tox->m, friendnumber)) {
        return 0;
    }

    uint8_t packet = PACKET_ID_ONLINE;
    return write_cryptpacket(tox->net_crypto,
                             toxconn_crypt_connection_id(tox->m->fr_c,
                                                         tox->m->friendlist[friendnumber].dev_list[device_num].friendcon_id),
                             &packet,
                             sizeof(packet),
                             0) != -1;
}

static int send_offline_packet(Tox *tox, int friendcon_id)
{
    uint8_t packet = PACKET_ID_OFFLINE;
    return write_cryptpacket(tox->net_crypto,
                             toxconn_crypt_connection_id(tox->m->fr_c,
                                                         friendcon_id),
                             &packet,
                             sizeof(packet), 0) != -1;
}

static int handle_status(void *object, int friend_num, int device_id, uint8_t status);
static int handle_packet(void *object, int friend_num, int device_id, uint8_t *temp, uint16_t len);
static int handle_custom_lossy_packet(void *object, int friend_num, int device_id, const uint8_t *packet, uint16_t length);

static int32_t init_new_friend(Tox *tox, const uint8_t *real_pk, uint8_t status)
{
    Messenger *m = tox->m;

    /* Resize the friend list if necessary. */
    if (realloc_friendlist(m, m->numfriends + 1) != 0)
        return FAERR_NOMEM;

    memset(&(m->friendlist[m->numfriends]), 0, sizeof(Friend));


    int friendcon_id = new_tox_conn(m->fr_c, real_pk);

    if (friendcon_id == -1)
        return FAERR_NOMEM;

    uint32_t i;

    for (i = 0; i <= m->numfriends; ++i) {
        if (m->friendlist[i].status == NOFRIEND) {
            if (realloc_dev_list(m, i, 1) != 0) {
                return FAERR_NOMEM;
            }

            m->friendlist[i].status = status;
            m->friendlist[i].friendrequest_lastsent = 0;
            m->friendlist[i].statusmessage_length = 0;
            m->friendlist[i].userstatus = USERSTATUS_NONE;
            m->friendlist[i].is_typing = 0;
            m->friendlist[i].message_id = 0;

            m->friendlist[i].dev_list[0].status = FDEV_CONFIRMED;
            m->friendlist[i].dev_list[0].friendcon_id = friendcon_id;
            id_copy(m->friendlist[i].dev_list[0].real_pk, real_pk);
            m->friendlist[i].dev_count = 1;

            toxconn_set_callbacks(m->fr_c, friendcon_id, MESSENGER_CALLBACK_INDEX,
                                  &handle_status, &handle_packet, &handle_custom_lossy_packet,
                                  tox, i, 0); /* device number always 0 for new friend */

            if (m->numfriends == i) {
                ++m->numfriends;
            }

            if (toxconn_is_connected(m->fr_c, friendcon_id) == TOXCONN_STATUS_CONNECTED) {
                m->friendlist[i].dev_list[0].status = FDEV_ONLINE;
                send_online_packet(tox, i, 0);
            }

            return i;
        }
    }

    return FAERR_NOMEM;
}

static int32_t init_new_device_friend(Tox *tox, uint32_t friend_number, const uint8_t *real_pk, uint8_t status)
{
    Friend *friend = &tox->m->friendlist[friend_number];
    uint32_t dev_count =  tox->m->friendlist[friend_number].dev_count;

    if (realloc_dev_list(tox->m, friend_number, dev_count + 1) != 0) {
        return FAERR_NOMEM;
    }

    memset(&(friend->dev_list[dev_count]), 0, sizeof(F_Device));

    int friendcon_id = new_tox_conn(tox->m->fr_c, real_pk);

    if (friendcon_id == -1) {
        return FAERR_NOMEM;
    }

    if (tox->m->friendlist[friend_number].status >= FRIEND_CONFIRMED) {
        uint8_t i;
        for (i = 1; i <= dev_count; ++i) {
            if (friend->dev_list[i].status == NO_FDEV) {
                friend->dev_list[i].friendcon_id = friendcon_id;
                friend->dev_list[i].status = status;
                id_copy(friend->dev_list[i].real_pk, real_pk);
                friend->dev_count++;
                toxconn_set_callbacks(tox->m->fr_c, friendcon_id, MESSENGER_CALLBACK_INDEX,
                                      &handle_status, &handle_packet, &handle_custom_lossy_packet,
                                      tox, friend_number, i);

                if (toxconn_is_connected(tox->m->fr_c, friendcon_id) == TOXCONN_STATUS_CONNECTED) {
                    friend->dev_list[i].status = FDEV_ONLINE;
                    send_online_packet(tox, friend_number, i);
                }
                printf("added new device to %i, at %i\n", friend_number, i);
                return i;
            }
        }
    }

    return FAERR_NOMEM;
}

/*
 * Add a friend.
 * Set the data that will be sent along with friend request.
 * Address is the address of the friend (returned by getaddress of the friend you wish to add) it must be FRIEND_ADDRESS_SIZE bytes.
 * data is the data and length is the length.
 *
 *  return the friend number if success.
 *  return FA_TOOLONG if message length is too long.
 *  return FAERR_NOMESSAGE if no message (message length must be >= 1 byte).
 *  return FAERR_OWNKEY if user's own key.
 *  return FAERR_ALREADYSENT if friend request already sent or already a friend.
 *  return FAERR_BADCHECKSUM if bad checksum in address.
 *  return FAERR_SETNEWNOSPAM if the friend was already there but the nospam was different.
 *  (the nospam for that friend was set to the new one).
 *  return FAERR_NOMEM if increasing the friend list size fails.
 */
int32_t m_addfriend(Tox *tox, const uint8_t *address, const uint8_t *data, uint16_t length)
{
    if (length > MAX_FRIEND_REQUEST_DATA_SIZE)
        return FAERR_TOOLONG;

    uint8_t real_pk[crypto_box_PUBLICKEYBYTES];
    id_copy(real_pk, address);

    if (!public_key_valid(real_pk))
        return FAERR_BADCHECKSUM;

    uint16_t check, checksum = address_checksum(address, FRIEND_ADDRESS_SIZE - sizeof(checksum));
    memcpy(&check, address + crypto_box_PUBLICKEYBYTES + sizeof(uint32_t), sizeof(check));

    if (check != checksum)
        return FAERR_BADCHECKSUM;

    if (length < 1)
        return FAERR_NOMESSAGE;

    if (id_equal(real_pk, tox->net_crypto->self_public_key))
        return FAERR_OWNKEY;

    int32_t friend_id = getfriend_id(tox->m, real_pk);

    if (friend_id != -1) {
        if (tox->m->friendlist[friend_id].status >= FRIEND_CONFIRMED)
            return FAERR_ALREADYSENT;

        uint32_t nospam;
        memcpy(&nospam, address + crypto_box_PUBLICKEYBYTES, sizeof(nospam));

        if (tox->m->friendlist[friend_id].friendrequest_nospam == nospam)
            return FAERR_ALREADYSENT;

        tox->m->friendlist[friend_id].friendrequest_nospam = nospam;
        return FAERR_SETNEWNOSPAM;
    }

    int32_t ret = init_new_friend(tox, real_pk, FRIEND_ADDED);

    if (ret < 0) {
        return ret;
    }

    tox->m->friendlist[ret].friendrequest_timeout = FRIENDREQUEST_TIMEOUT;
    memcpy(tox->m->friendlist[ret].info, data, length);
    tox->m->friendlist[ret].info_size = length;
    memcpy(&(tox->m->friendlist[ret].friendrequest_nospam), address + crypto_box_PUBLICKEYBYTES, sizeof(uint32_t));

    return ret;
}

int32_t m_addfriend_norequest(Tox *tox, const uint8_t *real_pk)
{
    if (getfriend_id(tox->m, real_pk) != -1)
        return FAERR_ALREADYSENT;

    if (!public_key_valid(real_pk))
        return FAERR_BADCHECKSUM;

    if (id_equal(real_pk, tox->net_crypto->self_public_key))
        return FAERR_OWNKEY;

    return init_new_friend(tox, real_pk, FRIEND_CONFIRMED);
}

/*
 * TODO: document this fxn
 *
 *
 *
 *
 */
int32_t m_add_device_to_friend(Tox *tox, const uint8_t *address, uint32_t friend_number)
{
    Messenger *m = tox->m;

    uint8_t real_pk[crypto_box_PUBLICKEYBYTES];
    id_copy(real_pk, address);

    if (!public_key_valid(real_pk)) {
        return FAERR_BADCHECKSUM;
    }

    uint16_t check, checksum = address_checksum(address, FRIEND_ADDRESS_SIZE - sizeof(checksum));
    memcpy(&check, address + crypto_box_PUBLICKEYBYTES + sizeof(uint32_t), sizeof(check));

    if (check != checksum) {
        return FAERR_BADCHECKSUM;
    }

    if (id_equal(real_pk, tox->net_crypto->self_public_key)) {
        return FAERR_OWNKEY;
    }

    int32_t friend_id = getfriend_id(tox->m, real_pk);

    if (friend_id != -1) {
        if (m->friendlist[friend_id].status >= FRIEND_CONFIRMED) {
            printf("already added this ID\n");
            return FAERR_ALREADYSENT;
        }

        uint32_t nospam;
        memcpy(&nospam, address + crypto_box_PUBLICKEYBYTES, sizeof(nospam));

        if (m->friendlist[friend_id].friendrequest_nospam == nospam) {
            return FAERR_ALREADYSENT;
        }

        m->friendlist[friend_id].friendrequest_nospam = nospam;
        return FAERR_SETNEWNOSPAM;
    }

    int32_t ret = init_new_device_friend(tox, friend_number, real_pk, FDEV_PENDING);

    return ret;
}

/*
 * TODO: document this fxn
 *
 *
 *
 *
 */
static int32_t m_add_device_to_friend_confirmed(Tox *tox, const uint8_t *real_pk, uint32_t friend_number)
{
    Messenger *m = tox->m;

    if (!public_key_valid(real_pk)) {
        return FAERR_BADCHECKSUM;
    }

    if (id_equal(real_pk, tox->net_crypto->self_public_key)) {
        return FAERR_OWNKEY;
    }

    int32_t friend_id = getfriend_id(tox->m, real_pk);

    if (friend_id != -1) {
        if (m->friendlist[friend_id].status >= FRIEND_CONFIRMED) {
            printf("Friend ID Already exists in list...\n");
            return FAERR_ALREADYSENT;
        }
    }

    return init_new_device_friend(tox, friend_number, real_pk, FDEV_CONFIRMED);
}

static int clear_receipts(Messenger *m, int32_t friendnumber)
{
    if (friend_not_valid(m, friendnumber))
        return -1;

    struct Receipts *receipts = m->friendlist[friendnumber].receipts_start;

    while (receipts) {
        struct Receipts *temp_r = receipts->next;
        free(receipts);
        receipts = temp_r;
    }

    m->friendlist[friendnumber].receipts_start = NULL;
    m->friendlist[friendnumber].receipts_end = NULL;
    return 0;
}

static int add_receipt(Tox *tox, int32_t friendnumber, uint32_t packet_num, uint32_t msg_id)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    struct Receipts *new = calloc(1, sizeof(struct Receipts));

    if (!new)
        return -1;

    new->packet_num = packet_num;
    new->msg_id = msg_id;

    if (!tox->m->friendlist[friendnumber].receipts_start) {
        tox->m->friendlist[friendnumber].receipts_start = new;
    } else {
        tox->m->friendlist[friendnumber].receipts_end->next = new;
    }

    tox->m->friendlist[friendnumber].receipts_end = new;
    new->next = NULL;
    return 0;
}

/*
 * return -1 on failure.
 * return 0 if packet was received.
 */
static int friend_received_packet(const Tox *tox, int32_t friendnumber, uint32_t number)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    return cryptpacket_received(tox->net_crypto,
                                toxconn_crypt_connection_id(tox->m->fr_c,
                                                            tox->m->friendlist[friendnumber].dev_list[0].friendcon_id),
                                number);
}

static int do_receipts(Tox *tox, int32_t friendnumber)
{
    Messenger *m = tox->m;

    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    struct Receipts *receipts = tox->m->friendlist[friendnumber].receipts_start;

    while (receipts) {
        struct Receipts *temp_r = receipts->next;

        if (friend_received_packet(tox, friendnumber, receipts->packet_num) == -1)
            break;

        if (tox->m->read_receipt)
            (*tox->m->read_receipt)(tox, friendnumber, receipts->msg_id, tox->m->read_receipt_userdata);

        free(receipts);
        tox->m->friendlist[friendnumber].receipts_start = temp_r;
        receipts = temp_r;
    }

    if (!tox->m->friendlist[friendnumber].receipts_start)
        tox->m->friendlist[friendnumber].receipts_end = NULL;

    return 0;
}

/* Remove a friend.
 *
 *  return 0 if success.
 *  return -1 if failure.
 */
int m_delfriend(Tox *tox, int32_t friendnumber)
{
    Messenger *m = tox->m;

    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    if (tox->m->friend_connectionstatuschange_internal)
        tox->m->friend_connectionstatuschange_internal(tox, friendnumber, 0, tox->m->friend_connectionstatuschange_internal_userdata);

    /* TODO for loop these */

    clear_receipts(tox->m, friendnumber);
    remove_request_received(&(tox->m->fr), tox->m->friendlist[friendnumber].dev_list[0].real_pk);
    toxconn_set_callbacks(tox->m->fr_c, tox->m->friendlist[friendnumber].dev_list[0].friendcon_id, MESSENGER_CALLBACK_INDEX, 0, 0, 0, 0, 0, 0);

    if (toxconn_is_connected(tox->m->fr_c, tox->m->friendlist[friendnumber].dev_list[0].friendcon_id) == TOXCONN_STATUS_CONNECTED) {
        send_offline_packet(tox, tox->m->friendlist[friendnumber].dev_list[0].friendcon_id);
    }

    kill_tox_conn(tox->m->fr_c, tox->m->friendlist[friendnumber].dev_list[0].friendcon_id);
    memset(&(tox->m->friendlist[friendnumber]), 0, sizeof(Friend));
    uint32_t i;

    for (i = tox->m->numfriends; i != 0; --i) {
        if (tox->m->friendlist[i - 1].status != NOFRIEND)
            break;
    }

    tox->m->numfriends = i;

    if (realloc_friendlist(m, tox->m->numfriends) != 0)
        return FAERR_NOMEM;

    return 0;
}

int m_get_friend_connectionstatus(const Tox *tox, int32_t friendnumber)
{
    Messenger *m = tox->m;

    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    if (m->friendlist[friendnumber].status == FRIEND_ONLINE) {
        _Bool direct_connected = 0;
        unsigned int num_online_relays = 0;
        crypto_connection_status(tox->net_crypto,
                                 toxconn_crypt_connection_id(m->fr_c,
                                                             m->friendlist[friendnumber].dev_list[0].friendcon_id),
                                 &direct_connected,
                                 &num_online_relays);

        if (direct_connected) {
            return CONNECTION_UDP;
        } else {
            if (num_online_relays) {
                return CONNECTION_TCP;
            } else {
                return CONNECTION_UNKNOWN;
            }
        }
    } else {
        return CONNECTION_NONE;
    }
}

int m_friend_exists(const Tox *tox, int32_t friendnumber)
{
    if (friend_not_valid(tox->m, friendnumber))
        return 0;

    return 1;
}

/* Send a message of type.
 *
 * return -1 if friend not valid.
 * return -2 if too large.
 * return -3 if friend not online.
 * return -4 if send failed (because queue is full).
 * return -5 if bad type.
 * return 0 if success.
 */
int m_send_message_generic(Tox *tox, int32_t friendnumber, uint8_t type, const uint8_t *message, uint32_t length,
                           uint32_t *message_id)
{
    Messenger *m = tox->m;

    if (type > MESSAGE_ACTION) {
        return -5;
    }

    if (friend_not_valid(tox->m, friendnumber)) {
        return -1;
    }

    if (length >= MAX_CRYPTO_DATA_SIZE) {
        return -2;
    }

    if (m->friendlist[friendnumber].status != FRIEND_ONLINE) {
        return -3;
    }

    uint8_t packet[length + 1];
    packet[0] = type + PACKET_ID_MESSAGE;

    if (length != 0) {
        memcpy(packet + 1, message, length);
    }

    uint8_t dev;
    int64_t packet_num = -1;

    for (dev = 0; dev < m->friendlist[friendnumber].dev_count; dev++) {
        if (m->friendlist[friendnumber].dev_list[dev].status == FDEV_ONLINE) {
            int crypt_con_id = toxconn_crypt_connection_id(m->fr_c,
                                                           m->friendlist[friendnumber].dev_list[dev].friendcon_id);
            int64_t this_packet_num = write_cryptpacket(tox->net_crypto, crypt_con_id, packet, length + 1, 0);

            if (this_packet_num == -1 && packet_num != -1) {
                continue;
            } else {
                packet_num = this_packet_num;
            }
        }
    }

    if (packet_num == -1) {
        return -4;
    }

    uint32_t msg_id = ++m->friendlist[friendnumber].message_id;

    add_receipt(tox, friendnumber, packet_num, msg_id);

    if (message_id) {
        *message_id = msg_id;
    }

    return 0;
}

/* Send a name packet to friendnumber.
 * length is the length with the NULL terminator.
 */
static int m_sendname(const Tox *tox, int32_t friendnumber, const uint8_t *name, uint16_t length)
{
    if (length > MAX_NAME_LENGTH)
        return 0;

    return write_cryptpacket_id(tox, friendnumber, PACKET_ID_NICKNAME, name, length, 0);
}

/* Set the name and name_length of a friend.
 *
 *  return 0 if success.
 *  return -1 if failure.
 */
int setfriendname(Messenger *m, int32_t friendnumber, const uint8_t *name, uint16_t length)
{
    if (friend_not_valid(m, friendnumber))
        return -1;

    if (length > MAX_NAME_LENGTH || length == 0)
        return -1;

    m->friendlist[friendnumber].name_length = length;
    memcpy(m->friendlist[friendnumber].name, name, length);
    return 0;
}

/* Set our nickname
 * name must be a string of maximum MAX_NAME_LENGTH length.
 * length must be at least 1 byte.
 * length is the length of name with the NULL terminator.
 *
 *  return 0 if success.
 *  return -1 if failure.
 */
int setname(Messenger *m, const uint8_t *name, uint16_t length)
{
    if (length > MAX_NAME_LENGTH)
        return -1;

    if (m->name_length == length && (length == 0 || memcmp(name, m->name, length) == 0))
        return 0;

    if (length)
        memcpy(m->name, name, length);

    m->name_length = length;
    uint32_t i;

    for (i = 0; i < m->numfriends; ++i)
        m->friendlist[i].name_sent = 0;

    return 0;
}

/* Get our nickname and put it in name.
 * name needs to be a valid memory location with a size of at least MAX_NAME_LENGTH bytes.
 *
 *  return the length of the name.
 */
uint16_t getself_name(const Messenger *m, uint8_t *name)
{
    if (name == NULL) {
        return 0;
    }

    memcpy(name, m->name, m->name_length);

    return m->name_length;
}

/* Get name of friendnumber and put it in name.
 * name needs to be a valid memory location with a size of at least MAX_NAME_LENGTH bytes.
 *
 *  return length of name if success.
 *  return -1 if failure.
 */
int getname(const Messenger *m, int32_t friendnumber, uint8_t *name)
{
    if (friend_not_valid(m, friendnumber))
        return -1;

    memcpy(name, m->friendlist[friendnumber].name, m->friendlist[friendnumber].name_length);
    return m->friendlist[friendnumber].name_length;
}

int m_get_name_size(const Tox *tox, int32_t friendnumber)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    return tox->m->friendlist[friendnumber].name_length;
}

int m_get_self_name_size(const Tox *tox)
{
    return tox->m->name_length;
}

int m_set_statusmessage(Tox *tox, const uint8_t *status, uint16_t length)
{
    if (length > MAX_STATUSMESSAGE_LENGTH)
        return -1;

    if (tox->m->statusmessage_length == length && (length == 0 || memcmp(tox->m->statusmessage, status, length) == 0))
        return 0;

    if (length)
        memcpy(tox->m->statusmessage, status, length);

    tox->m->statusmessage_length = length;

    uint32_t i;

    for (i = 0; i < tox->m->numfriends; ++i)
        tox->m->friendlist[i].statusmessage_sent = 0;

    return 0;
}

int m_set_userstatus(Tox *tox, uint8_t status)
{
    if (status >= USERSTATUS_INVALID)
        return -1;

    if (tox->m->userstatus == status)
        return 0;

    tox->m->userstatus = status;
    uint32_t i;

    for (i = 0; i < tox->m->numfriends; ++i)
        tox->m->friendlist[i].userstatus_sent = 0;

    return 0;
}

/* return the size of friendnumber's user status.
 * Guaranteed to be at most MAX_STATUSMESSAGE_LENGTH.
 */
int m_get_statusmessage_size(const Tox *tox, int32_t friendnumber)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    return tox->m->friendlist[friendnumber].statusmessage_length;
}

/*  Copy the user status of friendnumber into buf, truncating if needed to maxlen
 *  bytes, use m_get_statusmessage_size to find out how much you need to allocate.
 */
int m_copy_statusmessage(const Tox *tox, int32_t friendnumber, uint8_t *buf, uint32_t maxlen)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    int msglen = MIN(maxlen, tox->m->friendlist[friendnumber].statusmessage_length);

    memcpy(buf, tox->m->friendlist[friendnumber].statusmessage, msglen);
    memset(buf + msglen, 0, maxlen - msglen);
    return msglen;
}

/* return the size of friendnumber's user status.
 * Guaranteed to be at most MAX_STATUSMESSAGE_LENGTH.
 */
int m_get_self_statusmessage_size(const Tox *tox)
{
    return tox->m->statusmessage_length;
}

int m_copy_self_statusmessage(const Tox *tox, uint8_t *buf)
{
    memcpy(buf, tox->m->statusmessage, tox->m->statusmessage_length);
    return tox->m->statusmessage_length;
}

uint8_t m_get_userstatus(const Tox *tox, int32_t friendnumber)
{
    if (friend_not_valid(tox->m, friendnumber))
        return USERSTATUS_INVALID;

    uint8_t status = tox->m->friendlist[friendnumber].userstatus;

    if (status >= USERSTATUS_INVALID) {
        status = USERSTATUS_NONE;
    }

    return status;
}

uint8_t m_get_self_userstatus(const Tox *tox)
{
    return tox->m->userstatus;
}

uint64_t m_get_last_online(const Tox *tox, int32_t friendnumber)
{
    if (friend_not_valid(tox->m, friendnumber))
        return UINT64_MAX;

    return tox->m->friendlist[friendnumber].last_seen_time;
}

int m_set_usertyping(Tox *tox, int32_t friendnumber, uint8_t is_typing)

{
    if (is_typing != 0 && is_typing != 1)
        return -1;

    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    if (tox->m->friendlist[friendnumber].user_istyping == is_typing)
        return 0;

    tox->m->friendlist[friendnumber].user_istyping = is_typing;
    tox->m->friendlist[friendnumber].user_istyping_sent = 0;

    return 0;
}

int m_get_istyping(const Tox *tox, int32_t friendnumber)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    return tox->m->friendlist[friendnumber].is_typing;
}

static int send_statusmessage(const Tox *tox, int32_t friendnumber, const uint8_t *status, uint16_t length)
{
    return write_cryptpacket_id(tox, friendnumber, PACKET_ID_STATUSMESSAGE, status, length, 0);
}

static int send_userstatus(const Tox *tox, int32_t friendnumber, uint8_t status)
{
    return write_cryptpacket_id(tox, friendnumber, PACKET_ID_USERSTATUS, &status, sizeof(status), 0);
}

static int send_user_istyping(const Tox *tox, int32_t friendnumber, uint8_t is_typing)
{
    uint8_t typing = is_typing;
    return write_cryptpacket_id(tox, friendnumber, PACKET_ID_TYPING, &typing, sizeof(typing), 0);
}

int set_friend_statusmessage(const Messenger *m, int32_t friendnumber, const uint8_t *status, uint16_t length)
{
    if (friend_not_valid(m, friendnumber))
        return -1;

    if (length > MAX_STATUSMESSAGE_LENGTH)
        return -1;

    if (length)
        memcpy(m->friendlist[friendnumber].statusmessage, status, length);

    m->friendlist[friendnumber].statusmessage_length = length;
    return 0;
}

void set_friend_userstatus(const Messenger *m, int32_t friendnumber, uint8_t status)
{
    m->friendlist[friendnumber].userstatus = status;
}

static void set_friend_typing(const Messenger *m, int32_t friendnumber, uint8_t is_typing)
{
    m->friendlist[friendnumber].is_typing = is_typing;
}

/* Set the function that will be executed when a friend request is received. */
void m_callback_friendrequest(Tox *tox, void (*function)(Tox *tox, const uint8_t *, const uint8_t *, size_t,
                              void *), void *userdata)
{
    void (*handle_friendrequest)(void *, const uint8_t *, const uint8_t *, size_t, void *) = (void *)function;
    callback_friendrequest(&(tox->m->fr), handle_friendrequest, tox->m, userdata);
}

/* Set the function that will be executed when a message from a friend is received. */
void m_callback_friendmessage(Tox *tox, void (*function)(Tox *tox, uint32_t, unsigned int, const uint8_t *,
                              size_t, void *), void *userdata)
{
    tox->m->friend_message = function;
    tox->m->friend_message_userdata = userdata;
}

void m_callback_friend_list_change(Tox *tox, tox_friend_list_change_cb *function, void *userdata)
{
    tox->m->friend_list_change          = function;
    tox->m->friend_list_change_userdata = userdata;
}


void m_callback_namechange(Tox *tox, void (*function)(Tox *tox, uint32_t, const uint8_t *, size_t, void *),
                           void *userdata)
{
    tox->m->friend_namechange = function;
    tox->m->friend_namechange_userdata = userdata;
}

void m_callback_statusmessage(Tox *tox, void (*function)(Tox *tox, uint32_t, const uint8_t *, size_t, void *),
                              void *userdata)
{
    tox->m->friend_statusmessagechange = function;
    tox->m->friend_statusmessagechange_userdata = userdata;
}

void m_callback_userstatus(Tox *tox, void (*function)(Tox *tox, uint32_t, unsigned int, void *), void *userdata)
{
    tox->m->friend_userstatuschange = function;
    tox->m->friend_userstatuschange_userdata = userdata;
}

void m_callback_typingchange(Tox *tox, void(*function)(Tox *tox, uint32_t, _Bool, void *), void *userdata)
{
    tox->m->friend_typingchange = function;
    tox->m->friend_typingchange_userdata = userdata;
}

void m_callback_read_receipt(Tox *tox, void (*function)(Tox *tox, uint32_t, uint32_t, void *), void *userdata)
{
    tox->m->read_receipt = function;
    tox->m->read_receipt_userdata = userdata;
}

void m_callback_connectionstatus(Tox *tox, void (*function)(Tox *tox, uint32_t, unsigned int, void *),
                                 void *userdata)
{
    tox->m->friend_connectionstatuschange = function;
    tox->m->friend_connectionstatuschange_userdata = userdata;
}

void m_callback_core_connection(Tox *tox, void (*function)(Tox *tox, unsigned int, void *), void *userdata)
{
    tox->m->core_connection_change = function;
    tox->m->core_connection_change_userdata = userdata;
}

void m_callback_connectionstatus_internal_av(Tox *tox, void (*function)(Tox *tox, uint32_t, uint8_t, void *),
        void *userdata)
{
    tox->m->friend_connectionstatuschange_internal = function;
    tox->m->friend_connectionstatuschange_internal_userdata = userdata;
}

static void check_friend_tcp_udp(Tox *tox, int32_t friendnumber)
{
    int last_connection_udp_tcp = tox->m->friendlist[friendnumber].last_connection_udp_tcp;

    int ret = m_get_friend_connectionstatus(tox, friendnumber);

    if (ret == -1)
        return;

    if (ret == CONNECTION_UNKNOWN) {
        if (last_connection_udp_tcp == CONNECTION_UDP) {
            return;
        } else {
            ret = CONNECTION_TCP;
        }
    }

    if (last_connection_udp_tcp != ret) {
        if (tox->m->friend_connectionstatuschange)
            tox->m->friend_connectionstatuschange(tox, friendnumber, ret, tox->m->friend_connectionstatuschange_userdata);
    }

    tox->m->friendlist[friendnumber].last_connection_udp_tcp = ret;
}

static void break_files(const Messenger *m, int32_t friendnumber);
static void check_friend_connectionstatus(Tox *tox, int32_t friendnumber, uint8_t status)
{
    if (status == NOFRIEND)
        return;

    const uint8_t was_online = tox->m->friendlist[friendnumber].status == FRIEND_ONLINE;
    const uint8_t is_online = status == FRIEND_ONLINE;

    if (is_online != was_online) {
        if (was_online) {
            break_files(tox->m, friendnumber);
            clear_receipts(tox->m, friendnumber);
        } else {
            tox->m->friendlist[friendnumber].name_sent = 0;
            tox->m->friendlist[friendnumber].userstatus_sent = 0;
            tox->m->friendlist[friendnumber].statusmessage_sent = 0;
            tox->m->friendlist[friendnumber].user_istyping_sent = 0;
        }

        tox->m->friendlist[friendnumber].status = status;

        if (tox->m->friend_connectionstatuschange_internal)
            tox->m->friend_connectionstatuschange_internal(tox->m->tox, friendnumber, is_online,
                    tox->m->friend_connectionstatuschange_internal_userdata);
    }

    check_friend_tcp_udp(tox, friendnumber);
}

void set_friend_status(Tox *tox, int32_t friendnumber, uint8_t status)
{
    check_friend_connectionstatus(tox, friendnumber, status);
    tox->m->friendlist[friendnumber].status = status;
    switch (status) {
        case FRIEND_ADDED:
        case FRIEND_REQUESTED: {
            tox->m->friendlist[friendnumber].dev_list[0].status = FDEV_PENDING;
            break;
        }

        case FRIEND_CONFIRMED: {
            tox->m->friendlist[friendnumber].dev_list[0].status = FDEV_CONFIRMED;
            break;
        }

        case FRIEND_ONLINE: {
            tox->m->friendlist[friendnumber].dev_list[0].status = FDEV_ONLINE;
            break;
        }
    }
}

void set_device_status(Messenger *m, int32_t friendnumber, int32_t device_id, uint8_t status)
{
    m->friendlist[friendnumber].dev_list[device_id].status = status;

    if (status == FDEV_CONFIRMED) {

    } else if (status == FDEV_ONLINE) {

    }
}

static int write_cryptpacket_id(const Tox *tox, int32_t friendnumber, uint8_t packet_id, const uint8_t *data,
                                uint32_t length, uint8_t congestion_control)
{
    if (friend_not_valid(tox->m, friendnumber))
        return 0;

    if (length >= MAX_CRYPTO_DATA_SIZE || tox->m->friendlist[friendnumber].status != FRIEND_ONLINE)
        return 0;

    uint8_t packet[length + 1];
    packet[0] = packet_id;

    if (length != 0)
        memcpy(packet + 1, data, length);

    return write_cryptpacket(tox->net_crypto, toxconn_crypt_connection_id(tox->m->fr_c,
                             tox->m->friendlist[friendnumber].dev_list[0].friendcon_id), packet, length + 1, congestion_control) != -1;
}

/**********GROUP CHATS************/


/* Set the callback for group invites.
 *
 *  Function(Tox *tox, uint32_t friendnumber, uint8_t *data, uint16_t length)
 */
void m_callback_group_invite(Tox *tox, void (*function)(Tox *tox, uint32_t, const uint8_t *, uint16_t))
{
    tox->m->group_invite = function;
}


/* Send a group invite packet.
 *
 *  return 1 on success
 *  return 0 on failure
 */
int send_group_invite_packet(const Tox *tox, int32_t friendnumber, const uint8_t *data, uint16_t length)
{
    return write_cryptpacket_id(tox, friendnumber, PACKET_ID_INVITE_GROUPCHAT, data, length, 0);
}

/****************FILE SENDING*****************/


/* Set the callback for file send requests.
 *
 *  Function(Tox *tox, uint32_t friendnumber, uint32_t filenumber, uint32_t filetype, uint64_t filesize, uint8_t *filename, size_t filename_length, void *userdata)
 */
void callback_file_sendrequest(Tox *tox, void (*function)(Tox *tox,  uint32_t, uint32_t, uint32_t, uint64_t,
                               const uint8_t *, size_t, void *), void *userdata)
{
    tox->m->file_sendrequest = function;
    tox->m->file_sendrequest_userdata = userdata;
}

/* Set the callback for file control requests.
 *
 *  Function(Tox *tox, uint32_t friendnumber, uint32_t filenumber, unsigned int control_type, void *userdata)
 *
 */
void callback_file_control(Tox *tox, void (*function)(Tox *tox, uint32_t, uint32_t, unsigned int, void *),
                           void *userdata)
{
    tox->m->file_filecontrol = function;
    tox->m->file_filecontrol_userdata = userdata;
}

/* Set the callback for file data.
 *
 *  Function(Tox *tox, uint32_t friendnumber, uint32_t filenumber, uint64_t position, uint8_t *data, size_t length, void *userdata)
 *
 */
void callback_file_data(Tox *tox, void (*function)(Tox *tox, uint32_t, uint32_t, uint64_t, const uint8_t *,
                        size_t, void *), void *userdata)
{
    tox->m->file_filedata = function;
    tox->m->file_filedata_userdata = userdata;
}

/* Set the callback for file request chunk.
 *
 *  Function(Tox *tox, uint32_t friendnumber, uint32_t filenumber, uint64_t position, size_t length, void *userdata)
 *
 */
void callback_file_reqchunk(Tox *tox, void (*function)(Tox *tox, uint32_t, uint32_t, uint64_t, size_t, void *),
                            void *userdata)
{
    tox->m->file_reqchunk = function;
    tox->m->file_reqchunk_userdata = userdata;
}

#define MAX_FILENAME_LENGTH 255

/* Copy the file transfer file id to file_id
 *
 * return 0 on success.
 * return -1 if friend not valid.
 * return -2 if filenumber not valid
 */
int file_get_id(const Tox *tox, int32_t friendnumber, uint32_t filenumber, uint8_t *file_id)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    if (tox->m->friendlist[friendnumber].status != FRIEND_ONLINE)
        return -2;

    uint32_t temp_filenum;
    uint8_t send_receive, file_number;

    if (filenumber >= (1 << 16)) {
        send_receive = 1;
        temp_filenum = (filenumber >> 16) - 1;
    } else {
        send_receive = 0;
        temp_filenum = filenumber;
    }

    if (temp_filenum >= MAX_CONCURRENT_FILE_PIPES)
        return -2;

    file_number = temp_filenum;

    struct File_Transfers *ft;

    if (send_receive) {
        ft = &tox->m->friendlist[friendnumber].file_receiving[file_number];
    } else {
        ft = &tox->m->friendlist[friendnumber].file_sending[file_number];
    }

    if (ft->status == FILESTATUS_NONE)
        return -2;

    memcpy(file_id, ft->id, FILE_ID_LENGTH);
    return 0;
}

/* Send a file send request.
 * Maximum filename length is 255 bytes.
 *  return 1 on success
 *  return 0 on failure
 */
static int file_sendrequest(const Tox *tox, int32_t friendnumber, uint8_t filenumber, uint32_t file_type,
                            uint64_t filesize, const uint8_t *file_id, const uint8_t *filename, uint16_t filename_length)
{
    if (friend_not_valid(tox->m, friendnumber))
        return 0;

    if (filename_length > MAX_FILENAME_LENGTH)
        return 0;

    uint8_t packet[1 + sizeof(file_type) + sizeof(filesize) + FILE_ID_LENGTH + filename_length];
    packet[0] = filenumber;
    file_type = htonl(file_type);
    memcpy(packet + 1, &file_type, sizeof(file_type));
    host_to_net((uint8_t *)&filesize, sizeof(filesize));
    memcpy(packet + 1 + sizeof(file_type), &filesize, sizeof(filesize));
    memcpy(packet + 1 + sizeof(file_type) + sizeof(filesize), file_id, FILE_ID_LENGTH);

    if (filename_length) {
        memcpy(packet + 1 + sizeof(file_type) + sizeof(filesize) + FILE_ID_LENGTH, filename, filename_length);
    }

    return write_cryptpacket_id(tox, friendnumber, PACKET_ID_FILE_SENDREQUEST, packet, sizeof(packet), 0);
}

/* Send a file send request.
 * Maximum filename length is 255 bytes.
 *  return file number on success
 *  return -1 if friend not found.
 *  return -2 if filename length invalid.
 *  return -3 if no more file sending slots left.
 *  return -4 if could not send packet (friend offline).
 *
 */
long int new_filesender(const Tox *tox, int32_t friendnumber, uint32_t file_type, uint64_t filesize,
                        const uint8_t *file_id, const uint8_t *filename, uint16_t filename_length)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    if (filename_length > MAX_FILENAME_LENGTH)
        return -2;

    uint32_t i;

    for (i = 0; i < MAX_CONCURRENT_FILE_PIPES; ++i) {
        if (tox->m->friendlist[friendnumber].file_sending[i].status == FILESTATUS_NONE)
            break;
    }

    if (i == MAX_CONCURRENT_FILE_PIPES)
        return -3;

    if (file_sendrequest(tox, friendnumber, i, file_type, filesize, file_id, filename, filename_length) == 0)
        return -4;

    struct File_Transfers *ft = &tox->m->friendlist[friendnumber].file_sending[i];
    ft->status = FILESTATUS_NOT_ACCEPTED;
    ft->size = filesize;
    ft->transferred = 0;
    ft->requested = 0;
    ft->slots_allocated = 0;
    ft->paused = FILE_PAUSE_NOT;
    memcpy(ft->id, file_id, FILE_ID_LENGTH);

    ++tox->m->friendlist[friendnumber].num_sending_files;

    return i;
}

int send_file_control_packet(const Tox *tox, int32_t friendnumber, uint8_t send_receive, uint8_t filenumber,
                             uint8_t control_type, uint8_t *data, uint16_t data_length)
{
    if ((unsigned int)(1 + 3 + data_length) > MAX_CRYPTO_DATA_SIZE)
        return -1;

    uint8_t packet[3 + data_length];

    packet[0] = send_receive;
    packet[1] = filenumber;
    packet[2] = control_type;

    if (data_length) {
        memcpy(packet + 3, data, data_length);
    }

    return write_cryptpacket_id(tox, friendnumber, PACKET_ID_FILE_CONTROL, packet, sizeof(packet), 0);
}

/* Send a file control request.
 *
 *  return 0 on success
 *  return -1 if friend not valid.
 *  return -2 if friend not online.
 *  return -3 if file number invalid.
 *  return -4 if file control is bad.
 *  return -5 if file already paused.
 *  return -6 if resume file failed because it was only paused by the other.
 *  return -7 if resume file failed because it wasn't paused.
 *  return -8 if packet failed to send.
 */
int file_control(const Tox *tox, int32_t friendnumber, uint32_t filenumber, unsigned int control)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    if (tox->m->friendlist[friendnumber].status != FRIEND_ONLINE)
        return -2;

    uint32_t temp_filenum;
    uint8_t send_receive, file_number;

    if (filenumber >= (1 << 16)) {
        send_receive = 1;
        temp_filenum = (filenumber >> 16) - 1;
    } else {
        send_receive = 0;
        temp_filenum = filenumber;
    }

    if (temp_filenum >= MAX_CONCURRENT_FILE_PIPES)
        return -3;

    file_number = temp_filenum;

    struct File_Transfers *ft;

    if (send_receive) {
        ft = &tox->m->friendlist[friendnumber].file_receiving[file_number];
    } else {
        ft = &tox->m->friendlist[friendnumber].file_sending[file_number];
    }

    if (ft->status == FILESTATUS_NONE)
        return -3;

    if (control > FILECONTROL_KILL)
        return -4;

    if (control == FILECONTROL_PAUSE && ((ft->paused & FILE_PAUSE_US) || ft->status != FILESTATUS_TRANSFERRING))
        return -5;

    if (control == FILECONTROL_ACCEPT) {
        if (ft->status == FILESTATUS_TRANSFERRING) {
            if (!(ft->paused & FILE_PAUSE_US)) {
                if (ft->paused & FILE_PAUSE_OTHER) {
                    return -6;
                } else {
                    return -7;
                }
            }
        } else {
            if (ft->status != FILESTATUS_NOT_ACCEPTED)
                return -7;

            if (!send_receive)
                return -6;
        }
    }

    if (send_file_control_packet(tox, friendnumber, send_receive, file_number, control, 0, 0)) {
        if (control == FILECONTROL_KILL) {
            ft->status = FILESTATUS_NONE;

            if (send_receive == 0) {
                --tox->m->friendlist[friendnumber].num_sending_files;
            }
        } else if (control == FILECONTROL_PAUSE) {
            ft->paused |= FILE_PAUSE_US;
        } else if (control == FILECONTROL_ACCEPT) {
            ft->status = FILESTATUS_TRANSFERRING;

            if (ft->paused & FILE_PAUSE_US) {
                ft->paused ^=  FILE_PAUSE_US;
            }
        }
    } else {
        return -8;
    }

    return 0;
}

/* Send a seek file control request.
 *
 *  return 0 on success
 *  return -1 if friend not valid.
 *  return -2 if friend not online.
 *  return -3 if file number invalid.
 *  return -4 if not receiving file.
 *  return -5 if file status wrong.
 *  return -6 if position bad.
 *  return -8 if packet failed to send.
 */
int file_seek(const Tox *tox, int32_t friendnumber, uint32_t filenumber, uint64_t position)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    if (tox->m->friendlist[friendnumber].status != FRIEND_ONLINE)
        return -2;

    uint32_t temp_filenum;
    uint8_t send_receive, file_number;

    if (filenumber >= (1 << 16)) {
        send_receive = 1;
        temp_filenum = (filenumber >> 16) - 1;
    } else {
        return -4;
    }

    if (temp_filenum >= MAX_CONCURRENT_FILE_PIPES)
        return -3;

    file_number = temp_filenum;

    struct File_Transfers *ft;

    if (send_receive) {
        ft = &tox->m->friendlist[friendnumber].file_receiving[file_number];
    } else {
        ft = &tox->m->friendlist[friendnumber].file_sending[file_number];
    }

    if (ft->status == FILESTATUS_NONE)
        return -3;

    if (ft->status != FILESTATUS_NOT_ACCEPTED)
        return -5;

    if (position >= ft->size) {
        return -6;
    }

    uint64_t sending_pos = position;
    host_to_net((uint8_t *)&sending_pos, sizeof(sending_pos));

    if (send_file_control_packet(tox, friendnumber, send_receive, file_number, FILECONTROL_SEEK, (uint8_t *)&sending_pos,
                                 sizeof(sending_pos))) {
        ft->transferred = position;
    } else {
        return -8;
    }

    return 0;
}

/* return packet number on success.
 * return -1 on failure.
 */
static int64_t send_file_data_packet(const Tox *tox, int32_t friendnumber, uint8_t filenumber, const uint8_t *data,
                                     uint16_t length)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    uint8_t packet[2 + length];
    packet[0] = PACKET_ID_FILE_DATA;
    packet[1] = filenumber;

    if (length) {
        memcpy(packet + 2, data, length);
    }

    return write_cryptpacket(tox->net_crypto, toxconn_crypt_connection_id(tox->m->fr_c,
                             tox->m->friendlist[friendnumber].dev_list[0].friendcon_id), packet, sizeof(packet), 1);
}

#define MAX_FILE_DATA_SIZE (MAX_CRYPTO_DATA_SIZE - 2)
#define MIN_SLOTS_FREE (CRYPTO_MIN_QUEUE_LENGTH / 4)
/* Send file data.
 *
 *  return 0 on success
 *  return -1 if friend not valid.
 *  return -2 if friend not online.
 *  return -3 if filenumber invalid.
 *  return -4 if file transfer not transferring.
 *  return -5 if bad data size.
 *  return -6 if packet queue full.
 *  return -7 if wrong position.
 */
int file_data(const Tox *tox, int32_t friendnumber, uint32_t filenumber, uint64_t position, const uint8_t *data,
              uint16_t length)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    if (tox->m->friendlist[friendnumber].status != FRIEND_ONLINE)
        return -2;

    if (filenumber >= MAX_CONCURRENT_FILE_PIPES)
        return -3;

    struct File_Transfers *ft = &tox->m->friendlist[friendnumber].file_sending[filenumber];

    if (ft->status != FILESTATUS_TRANSFERRING)
        return -4;

    if (length > MAX_FILE_DATA_SIZE)
        return -5;

    if (ft->size - ft->transferred < length) {
        return -5;
    }

    if (ft->size != UINT64_MAX && length != MAX_FILE_DATA_SIZE && (ft->transferred + length) != ft->size) {
        return -5;
    }

    if (position != ft->transferred || (ft->requested <= position && ft->size != 0)) {
        return -7;
    }

    /* Prevent file sending from filling up the entire buffer preventing messages from being sent. TODO: remove */
    if (crypto_num_free_sendqueue_slots(tox->net_crypto, toxconn_crypt_connection_id(tox->m->fr_c,
                                        tox->m->friendlist[friendnumber].dev_list[0].friendcon_id)) < MIN_SLOTS_FREE)
        return -6;

    int64_t ret = send_file_data_packet(tox, friendnumber, filenumber, data, length);

    if (ret != -1) {
        //TODO record packet ids to check if other received complete file.
        ft->transferred += length;

        if (ft->slots_allocated) {
            --ft->slots_allocated;
        }

        if (length != MAX_FILE_DATA_SIZE || ft->size == ft->transferred) {
            ft->status = FILESTATUS_FINISHED;
            ft->last_packet_number = ret;
        }

        return 0;
    }

    return -6;

}

/* Give the number of bytes left to be sent/received.
 *
 *  send_receive is 0 if we want the sending files, 1 if we want the receiving.
 *
 *  return number of bytes remaining to be sent/received on success
 *  return 0 on failure
 */
uint64_t file_dataremaining(const Tox *tox, int32_t friendnumber, uint8_t filenumber, uint8_t send_receive)
{
    if (friend_not_valid(tox->m, friendnumber))
        return 0;

    if (send_receive == 0) {
        if (tox->m->friendlist[friendnumber].file_sending[filenumber].status == FILESTATUS_NONE)
            return 0;

        return tox->m->friendlist[friendnumber].file_sending[filenumber].size -
               tox->m->friendlist[friendnumber].file_sending[filenumber].transferred;
    } else {
        if (tox->m->friendlist[friendnumber].file_receiving[filenumber].status == FILESTATUS_NONE)
            return 0;

        return tox->m->friendlist[friendnumber].file_receiving[filenumber].size -
               tox->m->friendlist[friendnumber].file_receiving[filenumber].transferred;
    }
}

static void do_reqchunk_filecb(Tox *tox, int32_t friendnumber)
{
    if (!tox->m->friendlist[friendnumber].num_sending_files)
        return;

    int free_slots = crypto_num_free_sendqueue_slots(tox->net_crypto, toxconn_crypt_connection_id(tox->m->fr_c,
                     tox->m->friendlist[friendnumber].dev_list[0].friendcon_id));

    if (free_slots < MIN_SLOTS_FREE) {
        free_slots = 0;
    } else {
        free_slots -= MIN_SLOTS_FREE;
    }

    unsigned int i, num = tox->m->friendlist[friendnumber].num_sending_files;

    for (i = 0; i < MAX_CONCURRENT_FILE_PIPES; ++i) {
        struct File_Transfers *ft = &tox->m->friendlist[friendnumber].file_sending[i];

        if (ft->status != FILESTATUS_NONE) {
            --num;

            if (ft->status == FILESTATUS_FINISHED) {
                /* Check if file was entirely sent. */
                if (friend_received_packet(tox, friendnumber, ft->last_packet_number) == 0) {
                    if (tox->m->file_reqchunk)
                        (*tox->m->file_reqchunk)(tox, friendnumber, i, ft->transferred, 0, tox->m->file_reqchunk_userdata);

                    ft->status = FILESTATUS_NONE;
                    --tox->m->friendlist[friendnumber].num_sending_files;
                }
            }

            /* TODO: if file is too slow, switch to the next. */
            if (ft->slots_allocated > (unsigned int)free_slots) {
                free_slots = 0;
            } else {
                free_slots -= ft->slots_allocated;
            }
        }

        while (ft->status == FILESTATUS_TRANSFERRING && (ft->paused == FILE_PAUSE_NOT)) {
            if (max_speed_reached(tox->net_crypto, toxconn_crypt_connection_id(tox->m->fr_c,
                                  tox->m->friendlist[friendnumber].dev_list[0].friendcon_id))) {
                free_slots = 0;
            }

            if (free_slots == 0)
                break;

            uint16_t length = MAX_FILE_DATA_SIZE;

            if (ft->size == 0) {
                /* Send 0 data to friend if file is 0 length. */
                file_data(tox, friendnumber, i, 0, 0, 0);
                break;
            }

            if (ft->size == ft->requested) {
                break;
            }

            if (ft->size - ft->requested < length) {
                length = ft->size - ft->requested;
            }

            ++ft->slots_allocated;

            uint64_t position = ft->requested;
            ft->requested += length;

            if (tox->m->file_reqchunk)
                (*tox->m->file_reqchunk)(tox->m->tox, friendnumber, i, position, length, tox->m->file_reqchunk_userdata);

            --free_slots;

        }

        if (num == 0)
            break;
    }
}

/* Run this when the friend disconnects.
 *  Kill all current file transfers.
 */
static void break_files(const Messenger *m, int32_t friendnumber)
{
    uint32_t i;

    //TODO: Inform the client which file transfers get killed with a callback?
    for (i = 0; i < MAX_CONCURRENT_FILE_PIPES; ++i) {
        if (m->friendlist[friendnumber].file_sending[i].status != FILESTATUS_NONE)
            m->friendlist[friendnumber].file_sending[i].status = FILESTATUS_NONE;

        if (m->friendlist[friendnumber].file_receiving[i].status != FILESTATUS_NONE)
            m->friendlist[friendnumber].file_receiving[i].status = FILESTATUS_NONE;
    }
}

/* return -1 on failure, 0 on success.
 */
static int handle_filecontrol(Tox *tox, int32_t friendnumber, uint8_t receive_send, uint8_t filenumber,
                              uint8_t control_type, uint8_t *data, uint16_t length)
{
    if (receive_send > 1)
        return -1;

    if (control_type > FILECONTROL_SEEK)
        return -1;

    uint32_t real_filenumber = filenumber;
    struct File_Transfers *ft;

    if (receive_send == 0) {
        real_filenumber += 1;
        real_filenumber <<= 16;
        ft = &tox->m->friendlist[friendnumber].file_receiving[filenumber];
    } else {
        ft = &tox->m->friendlist[friendnumber].file_sending[filenumber];
    }

    if (ft->status == FILESTATUS_NONE) {
        /* File transfer doesn't exist, tell the other to kill it. */
        send_file_control_packet(tox, friendnumber, !receive_send, filenumber, FILECONTROL_KILL, 0, 0);
        return -1;
    }

    if (control_type == FILECONTROL_ACCEPT) {
        if (receive_send && ft->status == FILESTATUS_NOT_ACCEPTED) {
            ft->status = FILESTATUS_TRANSFERRING;
        } else {
            if (ft->paused & FILE_PAUSE_OTHER) {
                ft->paused ^= FILE_PAUSE_OTHER;
            } else {
                return -1;
            }
        }

        if (tox->m->file_filecontrol)
            (*tox->m->file_filecontrol)(tox->m->tox, friendnumber, real_filenumber, control_type, tox->m->file_filecontrol_userdata);
    } else if (control_type == FILECONTROL_PAUSE) {
        if ((ft->paused & FILE_PAUSE_OTHER) || ft->status != FILESTATUS_TRANSFERRING) {
            return -1;
        }

        ft->paused |= FILE_PAUSE_OTHER;

        if (tox->m->file_filecontrol)
            (*tox->m->file_filecontrol)(tox->m->tox, friendnumber, real_filenumber, control_type, tox->m->file_filecontrol_userdata);
    } else if (control_type == FILECONTROL_KILL) {

        if (tox->m->file_filecontrol)
            (*tox->m->file_filecontrol)(tox->m->tox, friendnumber, real_filenumber, control_type, tox->m->file_filecontrol_userdata);

        ft->status = FILESTATUS_NONE;

        if (receive_send) {
            --tox->m->friendlist[friendnumber].num_sending_files;
        }

    } else if (control_type == FILECONTROL_SEEK) {
        uint64_t position;

        if (length != sizeof(position)) {
            return -1;
        }

        /* seek can only be sent by the receiver to seek before resuming broken transfers. */
        if (ft->status != FILESTATUS_NOT_ACCEPTED || !receive_send) {
            return -1;
        }

        memcpy(&position, data, sizeof(position));
        net_to_host((uint8_t *) &position, sizeof(position));

        if (position >= ft->size) {
            return -1;
        }

        ft->transferred = ft->requested = position;
    } else {
        return -1;
    }

    return 0;
}

/**************************************/

/* Set the callback for msi packets.
 *
 *  Function(Tox *tox, int friendnumber, uint8_t *data, uint16_t length, void *userdata)
 */
void m_callback_msi_packet(Tox *tox, void (*function)(Tox *tox, uint32_t, const uint8_t *, uint16_t, void *),
                           void *userdata)
{
    tox->m->msi_packet = function;
    tox->m->msi_packet_userdata = userdata;
}

/* Send an msi packet.
 *
 *  return 1 on success
 *  return 0 on failure
 */
int m_msi_packet(const Tox *tox, int32_t friendnumber, const uint8_t *data, uint16_t length)
{
    return write_cryptpacket_id(tox, friendnumber, PACKET_ID_MSI, data, length, 0);
}

static int handle_custom_lossy_packet(void *object, int friend_num, int device_id, const uint8_t *packet, uint16_t length)
{
    Tox *tox = object;

    if (friend_not_valid(tox->m, friend_num))
        return 1;

    if (packet[0] < (PACKET_ID_LOSSY_RANGE_START + PACKET_LOSSY_AV_RESERVED)) {
        if (tox->m->friendlist[friend_num].lossy_rtp_packethandlers[packet[0] % PACKET_LOSSY_AV_RESERVED].function)
            return tox->m->friendlist[friend_num].lossy_rtp_packethandlers[packet[0] % PACKET_LOSSY_AV_RESERVED].function(
                       tox, friend_num, packet, length, tox->m->friendlist[friend_num].lossy_rtp_packethandlers[packet[0] %
                               PACKET_LOSSY_AV_RESERVED].object);

        return 1;
    }

    if (tox->m->lossy_packethandler)
        tox->m->lossy_packethandler(tox, friend_num, packet, length, tox->m->lossy_packethandler_userdata);

    return 1;
}

void custom_lossy_packet_registerhandler(Tox *tox, void (*packet_handler_callback)(Tox *tox,
        uint32_t friendnumber, const uint8_t *data, size_t len, void *object), void *object)
{
    tox->m->lossy_packethandler = packet_handler_callback;
    tox->m->lossy_packethandler_userdata = object;
}

int m_callback_rtp_packet(Tox *tox, int32_t friendnumber, uint8_t byte, int (*packet_handler_callback)(Tox *tox,
                          uint32_t friendnumber, const uint8_t *data, uint16_t len, void *object), void *object)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    if (byte < PACKET_ID_LOSSY_RANGE_START)
        return -1;

    if (byte >= (PACKET_ID_LOSSY_RANGE_START + PACKET_LOSSY_AV_RESERVED))
        return -1;

    tox->m->friendlist[friendnumber].lossy_rtp_packethandlers[byte % PACKET_LOSSY_AV_RESERVED].function =
        packet_handler_callback;
    tox->m->friendlist[friendnumber].lossy_rtp_packethandlers[byte % PACKET_LOSSY_AV_RESERVED].object = object;
    return 0;
}


int send_custom_lossy_packet(const Tox *tox, int32_t friendnumber, const uint8_t *data, uint32_t length)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    if (length == 0 || length > MAX_CRYPTO_DATA_SIZE)
        return -2;

    if (data[0] < PACKET_ID_LOSSY_RANGE_START)
        return -3;

    if (data[0] >= (PACKET_ID_LOSSY_RANGE_START + PACKET_ID_LOSSY_RANGE_SIZE))
        return -3;

    if (tox->m->friendlist[friendnumber].status != FRIEND_ONLINE)
        return -4;

    if (send_lossy_cryptpacket(tox->net_crypto, toxconn_crypt_connection_id(tox->m->fr_c,
                               tox->m->friendlist[friendnumber].dev_list[0].friendcon_id), data, length) == -1) {
        return -5;
    } else {
        return 0;
    }
}

static int handle_custom_lossless_packet(void *object, int friend_num, int device_id, const uint8_t *packet, uint16_t length)
{
    Tox *tox = object;

    if (friend_not_valid(tox->m, friend_num))
        return -1;

    if (packet[0] < PACKET_ID_LOSSLESS_RANGE_START)
        return -1;

    if (packet[0] >= (PACKET_ID_LOSSLESS_RANGE_START + PACKET_ID_LOSSLESS_RANGE_SIZE))
        return -1;

    if (tox->m->lossless_packethandler)
        tox->m->lossless_packethandler(tox->m->tox, friend_num, packet, length, tox->m->lossless_packethandler_userdata);

    return 1;
}

void custom_lossless_packet_registerhandler(Tox *tox, void (*packet_handler_callback)(Tox *tox,
        uint32_t friendnumber, const uint8_t *data, size_t len, void *object), void *object)
{
    tox->m->lossless_packethandler = packet_handler_callback;
    tox->m->lossless_packethandler_userdata = object;
}

int send_custom_lossless_packet(const Tox *tox, int32_t friendnumber, const uint8_t *data, uint32_t length)
{
    if (friend_not_valid(tox->m, friendnumber))
        return -1;

    if (length == 0 || length > MAX_CRYPTO_DATA_SIZE)
        return -2;

    if (data[0] < PACKET_ID_LOSSLESS_RANGE_START)
        return -3;

    if (data[0] >= (PACKET_ID_LOSSLESS_RANGE_START + PACKET_ID_LOSSLESS_RANGE_SIZE))
        return -3;

    if (tox->m->friendlist[friendnumber].status != FRIEND_ONLINE)
        return -4;

    if (write_cryptpacket(tox->net_crypto,
                          toxconn_crypt_connection_id(tox->m->fr_c, tox->m->friendlist[friendnumber].dev_list[0].friendcon_id),
                          data,
                          length,
                          1) == -1) {
        return -5;
    } else {
        return 0;
    }
}

/* Function to filter out some friend requests*/
static int friend_already_added(const uint8_t *real_pk, void *data)
{
    const Messenger *m = data;

    if (getfriend_id(m, real_pk) == -1)
        return 0;

    return -1;
}

/* Run this at startup. */
Messenger *new_messenger(Tox* tox, Messenger_Options *options, unsigned int *error)
{
    Messenger *m = calloc(1, sizeof(Messenger));

    if (error)
        *error = MESSENGER_ERROR_OTHER;

    if ( ! m )
        return NULL;

    m->fr_c = new_tox_conns(tox->onion_c);

    if (options->tcp_server_port) {
        m->tcp_server = new_TCP_server(options->ipv6enabled, 1, &options->tcp_server_port, tox->dht->self_secret_key, tox->onion);

        if (m->tcp_server == NULL) {
            kill_tox_conns(m->fr_c);
            free(m);

            if (error)
                *error = MESSENGER_ERROR_TCP_SERVER;

            return NULL;
        }
    }

    m->tox = tox;
    m->fr.crypto = tox->net_crypto;
    m->options = *options;
    friendreq_init(&(m->fr), m->fr_c);
    set_nospam(m->tox->net_crypto, random_int());
    set_filter_function(&(m->fr), &friend_already_added, m);

    if (error)
        *error = MESSENGER_ERROR_NONE;

    return m;
}

/* Run this before closing shop. */
void kill_messenger(Messenger *m)
{
    if (!m)
        return;

    uint32_t i;

    if (m->tcp_server) {
        kill_TCP_server(m->tcp_server);
    }

    kill_tox_conns(m->fr_c);

    for (i = 0; i < m->numfriends; ++i) {
        clear_receipts(m, i);
    }

    free(m->friendlist);
    free(m);
}

/* Check for and handle a timed-out friend request. If the request has
 * timed-out then the friend status is set back to FRIEND_ADDED.
 *   i: friendlist index of the timed-out friend
 *   t: time
 */
static void check_friend_request_timed_out(Tox *tox, uint32_t i, uint64_t t)
{
    Friend *f = &tox->m->friendlist[i];

    if (f->friendrequest_lastsent + f->friendrequest_timeout < t) {
        set_friend_status(tox, i, FRIEND_ADDED);
        /* Double the default timeout every time if friendrequest is assumed
         * to have been sent unsuccessfully.
         */
        f->friendrequest_timeout *= 2;
    }
}

static int handle_status(void *object, int friend_id, int device_id, uint8_t status)
{
    Tox *tox = object;

    if (status) { /* Went online. */
        set_device_status(tox->m, friend_id, device_id, FDEV_ONLINE);
        send_online_packet(tox, friend_id, device_id);
    } else { /* Went offline. */
        if (tox->m->friendlist[friend_id].status == FRIEND_ONLINE) {
            set_device_status(tox->m, friend_id, device_id, FDEV_CONFIRMED);
        }
    }

    return 0;
}

static int handle_packet(void *object, int friend_num, int device_id, uint8_t *temp, uint16_t len)
{
    if (len == 0)
        return -1;

    Tox *tox = object;
    Messenger *m = tox->m;
    uint8_t packet_id = temp[0];
    uint8_t *data = temp + 1;
    uint32_t data_length = len - 1;

    if (m->friendlist[friend_num].status != FRIEND_ONLINE) {
        if (packet_id == PACKET_ID_ONLINE && len == 1) {
            set_friend_status( tox, friend_num, FRIEND_ONLINE);
            set_device_status( tox->m, friend_num, device_id, FDEV_ONLINE);
            send_online_packet(tox, friend_num, device_id);
        } else {
            return -1;
        }
    }

    switch (packet_id) {
        case PACKET_ID_ONLINE: {
            if (len != 1) {
                return -1;
            }
            set_device_status(m, friend_num, device_id, FDEV_ONLINE);
            send_online_packet(tox, friend_num, device_id);
            break;
        }
        case PACKET_ID_OFFLINE: {
            if (data_length != 0)
                break;

            set_device_status(m, friend_num, device_id, FDEV_CONFIRMED);
            break;
        }

        case PACKET_ID_NICKNAME: {
            if (data_length > MAX_NAME_LENGTH)
                break;

            /* Make sure the NULL terminator is present. */
            uint8_t data_terminated[data_length + 1];
            memcpy(data_terminated, data, data_length);
            data_terminated[data_length] = 0;

            /* inform of namechange before we overwrite the old name */
            if (m->friend_namechange)
                m->friend_namechange(m->tox, friend_num, data_terminated, data_length, m->friend_namechange_userdata);

            memcpy(m->friendlist[friend_num].name, data_terminated, data_length);
            m->friendlist[friend_num].name_length = data_length;

            break;
        }

        case PACKET_ID_STATUSMESSAGE: {
            if (data_length > MAX_STATUSMESSAGE_LENGTH)
                break;

            /* Make sure the NULL terminator is present. */
            uint8_t data_terminated[data_length + 1];
            memcpy(data_terminated, data, data_length);
            data_terminated[data_length] = 0;

            if (m->friend_statusmessagechange)
                m->friend_statusmessagechange(m->tox, friend_num, data_terminated, data_length,
                                              m->friend_statusmessagechange_userdata);

            set_friend_statusmessage(m, friend_num, data_terminated, data_length);
            break;
        }

        case PACKET_ID_USERSTATUS: {
            if (data_length != 1)
                break;

            USERSTATUS status = data[0];

            if (status >= USERSTATUS_INVALID)
                break;

            if (m->friend_userstatuschange)
                m->friend_userstatuschange(m->tox, friend_num, status, m->friend_userstatuschange_userdata);

            set_friend_userstatus(m, friend_num, status);
            break;
        }

        case PACKET_ID_TYPING: {
            if (data_length != 1)
                break;

            _Bool typing = !!data[0];

            set_friend_typing(m, friend_num, typing);

            if (m->friend_typingchange)
                m->friend_typingchange(m->tox, friend_num, typing, m->friend_typingchange_userdata);

            break;
        }

        case PACKET_ID_MESSAGE:
        case PACKET_ID_ACTION: {
            if (data_length == 0)
                break;

            const uint8_t *message = data;
            uint16_t message_length = data_length;

            /* Make sure the NULL terminator is present. */
            uint8_t message_terminated[message_length + 1];
            memcpy(message_terminated, message, message_length);
            message_terminated[message_length] = 0;
            uint8_t type = packet_id - PACKET_ID_MESSAGE;

            if (m->friend_message)
                (*m->friend_message)(m->tox, friend_num, type, message_terminated, message_length, m->friend_message_userdata);

            break;
        }

        case PACKET_ID_INVITE_GROUPCHAT: {
            if (data_length == 0)
                break;

            if (m->group_invite)
                (*m->group_invite)(m->tox, friend_num, data, data_length);

            break;
        }

        case PACKET_ID_FILE_SENDREQUEST: {
            const unsigned int head_length = 1 + sizeof(uint32_t) + sizeof(uint64_t) + FILE_ID_LENGTH;

            if (data_length < head_length)
                break;

            uint8_t filenumber = data[0];

            if (filenumber >= MAX_CONCURRENT_FILE_PIPES)
                break;

            uint64_t filesize;
            uint32_t file_type;
            uint16_t filename_length = data_length - head_length;

            if (filename_length > MAX_FILENAME_LENGTH)
                break;

            memcpy(&file_type, data + 1, sizeof(file_type));
            file_type = ntohl(file_type);

            memcpy(&filesize, data + 1 + sizeof(uint32_t), sizeof(filesize));
            net_to_host((uint8_t *) &filesize, sizeof(filesize));
            struct File_Transfers *ft = &m->friendlist[friend_num].file_receiving[filenumber];

            if (ft->status != FILESTATUS_NONE)
                break;

            ft->status = FILESTATUS_NOT_ACCEPTED;
            ft->size = filesize;
            ft->transferred = 0;
            ft->paused = FILE_PAUSE_NOT;
            memcpy(ft->id, data + 1 + sizeof(uint32_t) + sizeof(uint64_t), FILE_ID_LENGTH);

            uint8_t filename_terminated[filename_length + 1];
            uint8_t *filename = NULL;

            if (filename_length) {
                /* Force NULL terminate file name. */
                memcpy(filename_terminated, data + head_length, filename_length);
                filename_terminated[filename_length] = 0;
                filename = filename_terminated;
            }

            uint32_t real_filenumber = filenumber;
            real_filenumber += 1;
            real_filenumber <<= 16;

            if (m->file_sendrequest)
                (*m->file_sendrequest)(m->tox, friend_num, real_filenumber, file_type, filesize, filename, filename_length,
                                       m->file_sendrequest_userdata);

            break;
        }

        case PACKET_ID_FILE_CONTROL: {
            if (data_length < 3)
                break;

            uint8_t send_receive = data[0];
            uint8_t filenumber = data[1];
            uint8_t control_type = data[2];

            if (filenumber >= MAX_CONCURRENT_FILE_PIPES)
                break;

            if (handle_filecontrol(tox, friend_num, send_receive, filenumber, control_type, data + 3, data_length - 3) == -1)
                break;

            break;
        }

        case PACKET_ID_FILE_DATA: {
            if (data_length < 1)
                break;

            uint8_t filenumber = data[0];

            if (filenumber >= MAX_CONCURRENT_FILE_PIPES)
                break;

            struct File_Transfers *ft = &m->friendlist[friend_num].file_receiving[filenumber];

            if (ft->status != FILESTATUS_TRANSFERRING)
                break;

            uint64_t position = ft->transferred;
            uint32_t real_filenumber = filenumber;
            real_filenumber += 1;
            real_filenumber <<= 16;
            uint16_t file_data_length = (data_length - 1);
            uint8_t *file_data;

            if (file_data_length == 0) {
                file_data = NULL;
            } else {
                file_data = data + 1;
            }

            /* Prevent more data than the filesize from being passed to clients. */
            if ((ft->transferred + file_data_length) > ft->size) {
                file_data_length = ft->size - ft->transferred;
            }

            if (m->file_filedata)
                (*m->file_filedata)(m->tox, friend_num, real_filenumber, position, file_data, file_data_length, m->file_filedata_userdata);

            ft->transferred += file_data_length;

            if (file_data_length && (ft->transferred >= ft->size || file_data_length != MAX_FILE_DATA_SIZE)) {
                file_data_length = 0;
                file_data = NULL;
                position = ft->transferred;

                /* Full file received. */
                if (m->file_filedata)
                    (*m->file_filedata)(m->tox, friend_num, real_filenumber, position, file_data, file_data_length, m->file_filedata_userdata);
            }

            /* Data is zero, filetransfer is over. */
            if (file_data_length == 0) {
                ft->status = FILESTATUS_NONE;
            }

            break;
        }

        case PACKET_ID_MSI: {
            if (data_length == 0)
                break;

            if (m->msi_packet)
                (*m->msi_packet)(m->tox, friend_num, data, data_length, m->msi_packet_userdata);

            break;
        }

        default: {
            handle_custom_lossless_packet(object, friend_num, device_id, temp, len);
            break;
        }
    }

    return 0;
}

void do_friends(Tox *tox)
{
    uint32_t i;
    uint64_t temp_time = unix_time();

    for (i = 0; i < tox->m->numfriends; ++i) {
        if (tox->m->friendlist[i].status == FRIEND_ADDED) {
            int fr = send_toxconn_request_pkt(tox->m->fr_c, tox->m->friendlist[i].dev_list[0].friendcon_id,
                                               tox->m->friendlist[i].friendrequest_nospam,
                                               tox->m->friendlist[i].info,
                                               tox->m->friendlist[i].info_size);

            if (fr >= 0) {
                set_friend_status(tox, i, FRIEND_REQUESTED);
                tox->m->friendlist[i].friendrequest_lastsent = temp_time;
            }
        }

        if (tox->m->friendlist[i].status == FRIEND_REQUESTED
                || tox->m->friendlist[i].status == FRIEND_CONFIRMED) { /* friend is not online. */
            if (tox->m->friendlist[i].status == FRIEND_REQUESTED) {
                /* If we didn't connect to friend after successfully sending him a friend request the request is deemed
                 * unsuccessful so we set the status back to FRIEND_ADDED and try again.
                 */
                check_friend_request_timed_out(tox, i, temp_time);
            }
        }

        if (tox->m->friendlist[i].status == FRIEND_ONLINE) { /* friend is online. */
            if (tox->m->friendlist[i].name_sent == 0) {
                if (m_sendname(tox, i, tox->m->name, tox->m->name_length))
                    tox->m->friendlist[i].name_sent = 1;
            }

            if (tox->m->friendlist[i].statusmessage_sent == 0) {
                if (send_statusmessage(tox, i, tox->m->statusmessage, tox->m->statusmessage_length))
                    tox->m->friendlist[i].statusmessage_sent = 1;
            }

            if (tox->m->friendlist[i].userstatus_sent == 0) {
                if (send_userstatus(tox, i, tox->m->userstatus))
                    tox->m->friendlist[i].userstatus_sent = 1;
            }

            if (tox->m->friendlist[i].user_istyping_sent == 0) {
                if (send_user_istyping(tox, i, tox->m->friendlist[i].user_istyping))
                    tox->m->friendlist[i].user_istyping_sent = 1;
            }

            check_friend_tcp_udp(tox, i);
            do_receipts(tox, i);
            do_reqchunk_filecb(tox, i);

            tox->m->friendlist[i].last_seen_time = (uint64_t) time(NULL);
        }
    }
}

static void connection_status_cb(Tox *tox)
{
    unsigned int conn_status = onion_connection_status(tox->onion_c);

    if (conn_status != tox->m->last_connection_status) {
        if (tox->m->core_connection_change)
            (*tox->m->core_connection_change)(tox->m->tox, conn_status, tox->m->core_connection_change_userdata);

        tox->m->last_connection_status = conn_status;
    }
}


#ifdef TOX_LOGGER
#define DUMPING_CLIENTS_FRIENDS_EVERY_N_SECONDS 60UL
static time_t lastdump = 0;
static char IDString[crypto_box_PUBLICKEYBYTES * 2 + 1];
static char *ID2String(const uint8_t *pk)
{
    uint32_t i;

    for (i = 0; i < crypto_box_PUBLICKEYBYTES; i++)
        sprintf(&IDString[i * 2], "%02X", pk[i]);

    IDString[crypto_box_PUBLICKEYBYTES * 2] = 0;
    return IDString;
}
#endif

/* Minimum messenger run interval in ms */
#define MIN_RUN_INTERVAL 50

/* Return the time in milliseconds before do_messenger() should be called again
 * for optimal performance.
 *
 * returns time (in ms) before the next do_messenger() needs to be run on success.
 */
uint32_t messenger_run_interval(const Tox *tox)
{
    uint32_t crypto_interval = crypto_run_interval(tox->net_crypto);

    if (crypto_interval > MIN_RUN_INTERVAL) {
        return MIN_RUN_INTERVAL;
    } else {
        return crypto_interval;
    }
}

/* The main loop that needs to be run at least 20 times per second. */
void do_messenger(Tox *tox)
{
    // Add the TCP relays, but only if this is the first time calling do_messenger
    if (tox->m->has_added_relays == 0) {
        tox->m->has_added_relays = 1;

        int i;

        for (i = 0; i < NUM_SAVED_TCP_RELAYS; ++i) {
            add_tcp_relay(tox->net_crypto, tox->m->loaded_relays[i].ip_port, tox->m->loaded_relays[i].public_key);
        }

        if (tox->m->tcp_server) {
            /* Add self tcp server. */
            IP_Port local_ip_port;
            local_ip_port.port = tox->m->options.tcp_server_port;
            local_ip_port.ip.family = AF_INET;
            local_ip_port.ip.ip4.uint32 = INADDR_LOOPBACK;
            add_tcp_relay(tox->net_crypto, local_ip_port, tox->m->tcp_server->public_key);
        }
    }

    unix_time_update();

    if (!tox->m->options.udp_disabled) {
        networking_poll(tox->net);
        do_DHT(tox->dht);
    }

    if (tox->m->tcp_server) {
        do_TCP_server(tox->m->tcp_server);
    }

    do_net_crypto(tox->net_crypto);
    do_onion_client(tox->onion_c);
    do_tox_connections(tox->m->fr_c);
    do_friends(tox);
    connection_status_cb(tox);

#ifdef TOX_LOGGER

    if (unix_time() > lastdump + DUMPING_CLIENTS_FRIENDS_EVERY_N_SECONDS) {

        lastdump = unix_time();
        uint32_t client, last_pinged;

        for (client = 0; client < LCLIENT_LIST; client++) {
            Client_data *cptr = &m->dht->close_clientlist[client];
            IPPTsPng *assoc = NULL;
            uint32_t a;

            for (a = 0, assoc = &cptr->assoc4; a < 2; a++, assoc = &cptr->assoc6)
                if (ip_isset(&assoc->ip_port.ip)) {
                    last_pinged = lastdump - assoc->last_pinged;

                    if (last_pinged > 999)
                        last_pinged = 999;

                    LOGGER_TRACE("C[%2u] %s:%u [%3u] %s",
                                 client, ip_ntoa(&assoc->ip_port.ip), ntohs(assoc->ip_port.port),
                                 last_pinged, ID2String(cptr->public_key));
                }
        }


        uint32_t friend, dhtfriend;

        /* dht contains additional "friends" (requests) */
        uint32_t num_dhtfriends = m->dht->num_friends;
        int32_t m2dht[num_dhtfriends];
        int32_t dht2m[num_dhtfriends];

        for (friend = 0; friend < num_dhtfriends; friend++) {
            m2dht[friend] = -1;
            dht2m[friend] = -1;

            if (friend >= m->numfriends)
                continue;

            for (dhtfriend = 0; dhtfriend < m->dht->num_friends; dhtfriend++)
                if (id_equal(m->friendlist[friend].real_pk, m->dht->friends_list[dhtfriend].public_key)) {
                    m2dht[friend] = dhtfriend;
                    break;
                }
        }

        for (friend = 0; friend < num_dhtfriends; friend++)
            if (m2dht[friend] >= 0)
                dht2m[m2dht[friend]] = friend;

        if (m->numfriends != m->dht->num_friends) {
            LOGGER_TRACE("Friend num in DHT %u != friend num in msger %u\n", m->dht->num_friends, m->numfriends);
        }

        Friend *msgfptr;
        DHT_Friend *dhtfptr;

        for (friend = 0; friend < num_dhtfriends; friend++) {
            if (dht2m[friend] >= 0)
                msgfptr = &m->friendlist[dht2m[friend]];
            else
                msgfptr = NULL;

            dhtfptr = &m->dht->friends_list[friend];

            if (msgfptr) {
                LOGGER_TRACE("F[%2u:%2u] <%s> %s",
                             dht2m[friend], friend, msgfptr->name,
                             ID2String(msgfptr->real_pk));
            } else {
                LOGGER_TRACE("F[--:%2u] %s", friend, ID2String(dhtfptr->public_key));
            }

            for (client = 0; client < MAX_FRIEND_CLIENTS; client++) {
                Client_data *cptr = &dhtfptr->client_list[client];
                IPPTsPng *assoc = NULL;
                uint32_t a;

                for (a = 0, assoc = &cptr->assoc4; a < 2; a++, assoc = &cptr->assoc6)
                    if (ip_isset(&assoc->ip_port.ip)) {
                        last_pinged = lastdump - assoc->last_pinged;

                        if (last_pinged > 999)
                            last_pinged = 999;

                        LOGGER_TRACE("F[%2u] => C[%2u] %s:%u [%3u] %s",
                                     friend, client, ip_ntoa(&assoc->ip_port.ip),
                                     ntohs(assoc->ip_port.port), last_pinged,
                                     ID2String(cptr->public_key));
                    }
            }
        }
    }

#endif /* TOX_LOGGER */
}

#define SAVED_FRIEND_REQUEST_SIZE 1024

struct SAVED_DEVICE {
    uint8_t  device_status;
    uint8_t  real_pk[crypto_box_PUBLICKEYBYTES];
};

struct SAVED_FRIEND {
    uint8_t  status;
    uint8_t  info[SAVED_FRIEND_REQUEST_SIZE]; // the data that is sent during the friend requests we do.
    uint16_t info_size; // Length of the info.
    uint8_t  name[MAX_NAME_LENGTH];
    uint16_t name_length;
    uint8_t  statusmessage[MAX_STATUSMESSAGE_LENGTH];
    uint16_t statusmessage_length;
    uint8_t  userstatus;
    uint32_t friendrequest_nospam;
    uint64_t last_seen_time;

    uint8_t  dev_count;
    struct   SAVED_DEVICE device[];
};

/* On-disk friend format for pre multi-device toxcore versions */
struct SAVED_OLDFRIEND {
    uint8_t status;
    uint8_t real_pk[crypto_box_PUBLICKEYBYTES];
    uint8_t info[SAVED_FRIEND_REQUEST_SIZE]; // the data that is sent during the friend requests we do.
    uint16_t info_size; // Length of the info.
    uint8_t name[MAX_NAME_LENGTH];
    uint16_t name_length;
    uint8_t statusmessage[MAX_STATUSMESSAGE_LENGTH];
    uint16_t statusmessage_length;
    uint8_t userstatus;
    uint32_t friendrequest_nospam;
    uint64_t last_seen_time;
};

static uint32_t count_devices(const Messenger *m)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < m->numfriends; ++i) {
        if (m->friendlist[i].status > 0) {
            total += m->friendlist[i].dev_count;
        }
    }

    return total;
}

static uint32_t saved_friendslist_size(const Messenger *m)
{
    return sizeof(uint8_t) + count_friendlist(m) * sizeof(struct SAVED_FRIEND)
                           + count_devices(m)    * sizeof(struct SAVED_DEVICE);
}

static uint32_t friends_list_save(const Tox *tox, uint8_t *data)
{
    uint32_t i, device, device_i, friend_total = 0, device_total = 0;

    uint8_t version = 1; /* Should be the latest version understood by friends_list_load */
    data[0] = version;
    data++;

    for (i = 0; i < tox->m->numfriends; i++) {
        /* For each friend is the list */
        if (tox->m->friendlist[i].status > 0) {
            struct SAVED_FRIEND temp;
            struct SAVED_DEVICE devices[tox->m->friendlist[i].dev_count];

            memset(&temp, 0, sizeof(struct SAVED_FRIEND));
            memset(&devices, 0, sizeof(struct SAVED_DEVICE) * tox->m->friendlist[i].dev_count);
            device_i = 0;

            temp.status = tox->m->friendlist[i].status;

            for(device = 0; device < tox->m->friendlist[i].dev_count; ++device) {
                /* For each device in the friend list */
                if (tox->m->friendlist[i].dev_list[device].status) {
                    devices[device_i].device_status = tox->m->friendlist[i].dev_list[device].status;
                    memcpy(devices[device_i].real_pk, tox->m->friendlist[i].dev_list[device].real_pk, crypto_box_PUBLICKEYBYTES);
                    ++device_i;
                    ++device_total;
                    ++temp.dev_count;
                }
            }

            if (temp.status < 3) {
                if (tox->m->friendlist[i].info_size > SAVED_FRIEND_REQUEST_SIZE) {
                    memcpy(temp.info, tox->m->friendlist[i].info, SAVED_FRIEND_REQUEST_SIZE);
                } else {
                    memcpy(temp.info, tox->m->friendlist[i].info, tox->m->friendlist[i].info_size);
                }

                temp.info_size = htons(tox->m->friendlist[i].info_size);
                temp.friendrequest_nospam = tox->m->friendlist[i].friendrequest_nospam;
            } else {
                memcpy(temp.name, tox->m->friendlist[i].name, tox->m->friendlist[i].name_length);
                temp.name_length = htons(tox->m->friendlist[i].name_length);
                memcpy(temp.statusmessage, tox->m->friendlist[i].statusmessage, tox->m->friendlist[i].statusmessage_length);
                temp.statusmessage_length = htons(tox->m->friendlist[i].statusmessage_length);
                temp.userstatus = tox->m->friendlist[i].userstatus;

                uint8_t last_seen_time[sizeof(uint64_t)];
                memcpy(last_seen_time, &tox->m->friendlist[i].last_seen_time, sizeof(uint64_t));
                host_to_net(last_seen_time, sizeof(uint64_t));
                memcpy(&temp.last_seen_time, last_seen_time, sizeof(uint64_t));
            }

            memcpy(data, &temp, sizeof(struct SAVED_FRIEND));
            data += sizeof(struct SAVED_FRIEND);
            memcpy(data, &devices, sizeof(struct SAVED_DEVICE) * device_i);
            data += sizeof(struct SAVED_DEVICE) * device_i;

            ++friend_total;
        }
    }

    return sizeof(version) + friend_total * sizeof(struct SAVED_FRIEND)
                           + device_total * sizeof(struct SAVED_DEVICE);
}

static int oldfriends_list_load(Messenger *m, const uint8_t *data, uint32_t length)
{
    if (length % sizeof(struct SAVED_OLDFRIEND) != 0) {
        return -1;
    }

    uint32_t num = length / sizeof(struct SAVED_OLDFRIEND);
    uint32_t i;

    for (i = 0; i < num; ++i) {
        struct SAVED_OLDFRIEND temp;
        memcpy(&temp, data + i * sizeof(struct SAVED_OLDFRIEND), sizeof(struct SAVED_OLDFRIEND));

        if (temp.status >= 3) {
            int fnum = m_addfriend_norequest(m->tox, temp.real_pk);

            if (fnum < 0)
                continue;

            setfriendname(m, fnum, temp.name, ntohs(temp.name_length));
            set_friend_statusmessage(m, fnum, temp.statusmessage, ntohs(temp.statusmessage_length));
            set_friend_userstatus(m, fnum, temp.userstatus);
            uint8_t last_seen_time[sizeof(uint64_t)];
            memcpy(last_seen_time, &temp.last_seen_time, sizeof(uint64_t));
            net_to_host(last_seen_time, sizeof(uint64_t));
            memcpy(&m->friendlist[fnum].last_seen_time, last_seen_time, sizeof(uint64_t));
        } else if (temp.status != 0) {
            /* TODO: This is not a good way to do this. */
            uint8_t address[FRIEND_ADDRESS_SIZE];
            id_copy(address, temp.real_pk);
            memcpy(address + crypto_box_PUBLICKEYBYTES, &(temp.friendrequest_nospam), sizeof(uint32_t));
            uint16_t checksum = address_checksum(address, FRIEND_ADDRESS_SIZE - sizeof(checksum));
            memcpy(address + crypto_box_PUBLICKEYBYTES + sizeof(uint32_t), &checksum, sizeof(checksum));
            m_addfriend(m->tox, address, temp.info, ntohs(temp.info_size));
        }
    }

    return num;
}

static int friends_list_load(Tox *tox, const uint8_t *data, uint32_t length)
{
    if (length < sizeof(uint8_t)) {
        return -1;
    }

    uint8_t version = data[0];
    data++;
    length--;

    if (version == 1) {
        uint32_t mod = length % sizeof(struct SAVED_FRIEND);
        if (mod % sizeof(struct SAVED_DEVICE)) {
            return -1;
        }

        int friends = 0;
        uint32_t i, device;

        while (length) {
            struct SAVED_FRIEND temp;
            memcpy(&temp, data, sizeof(struct SAVED_FRIEND));
            data += sizeof(struct SAVED_FRIEND);
            length -= sizeof(struct SAVED_FRIEND);

            struct SAVED_DEVICE dev;
            memcpy(&dev, data, sizeof(struct SAVED_DEVICE));
            data += sizeof(struct SAVED_DEVICE);
            length -= sizeof(struct SAVED_DEVICE);

            if (temp.status >= 3) {
                int fnum = m_addfriend_norequest(tox, dev.real_pk);

                if (fnum < 0) {
                    continue;
                }

                setfriendname(tox->m, fnum, temp.name, ntohs(temp.name_length));
                set_friend_statusmessage(tox->m, fnum, temp.statusmessage, ntohs(temp.statusmessage_length));
                set_friend_userstatus(tox->m, fnum, temp.userstatus);
                uint8_t last_seen_time[sizeof(uint64_t)];
                memcpy(last_seen_time, &temp.last_seen_time, sizeof(uint64_t));
                net_to_host(last_seen_time, sizeof(uint64_t));
                memcpy(&tox->m->friendlist[fnum].last_seen_time, last_seen_time, sizeof(uint64_t));

                for (device = 1; device < temp.dev_count; ++device) {
                    memcpy(&dev, data, sizeof(struct SAVED_DEVICE));
                    data += sizeof(struct SAVED_DEVICE);
                    length -= sizeof(struct SAVED_DEVICE);

                    if (dev.device_status && public_key_valid(dev.real_pk)) {
                        m_add_device_to_friend_confirmed(tox, dev.real_pk, fnum);
                    }
                }
            } else if (temp.status != 0) {
                /* TODO: This is not a good way to do this. */
                /* TODO: Do we want to add devices for unconfirmed friends?
                    -- Yes, that why if the "primary" device isn't online, you can manually add a 2nd device and still
                       connect to that friend */
                uint8_t address[FRIEND_ADDRESS_SIZE];
                id_copy(address, dev.real_pk);
                memcpy(address + crypto_box_PUBLICKEYBYTES, &(temp.friendrequest_nospam), sizeof(uint32_t));
                uint16_t checksum = address_checksum(address, FRIEND_ADDRESS_SIZE - sizeof(checksum));
                memcpy(address + crypto_box_PUBLICKEYBYTES + sizeof(uint32_t), &checksum, sizeof(checksum));
                m_addfriend(tox, address, temp.info, ntohs(temp.info_size));
            }
            ++friends;
        }

        return friends;
    } else {
        return -1;
    }
}

/*  return size of the messenger data (for saving) */
uint32_t messenger_size(const Tox *tox)
{
    if (!tox->m)
        return 0;

    uint32_t sizesubhead = save_subheader_size();
    return     sizesubhead + saved_friendslist_size(tox->m)         // Friendlist itself.
             + sizesubhead + tox->m->name_length                    // Own nickname.
             + sizesubhead + tox->m->statusmessage_length           // status message
             + sizesubhead + 1                                      // status
             + sizesubhead + NUM_SAVED_TCP_RELAYS * packed_node_size(TCP_INET6) //TCP relays
             ;
}

/* Save the messenger in data of size Messenger_size(). */
uint8_t *messenger_save(const Tox *tox, uint8_t *data)
{
    memset(data, 0, messenger_size(tox));

    uint32_t len;

    len = saved_friendslist_size(tox->m);
    data = save_write_subheader(data, len, SAVE_STATE_TYPE_FRIENDS, SAVE_STATE_COOKIE_TYPE);
    friends_list_save(tox, data);
    data += len;

    len = tox->m->name_length;
    data = save_write_subheader(data, len, SAVE_STATE_TYPE_NAME, SAVE_STATE_COOKIE_TYPE);
    memcpy(data, tox->m->name, len);
    data += len;

    len = tox->m->statusmessage_length;
    data = save_write_subheader(data, len, SAVE_STATE_TYPE_STATUSMESSAGE, SAVE_STATE_COOKIE_TYPE);
    memcpy(data, tox->m->statusmessage, len);
    data += len;

    len = 1;
    data = save_write_subheader(data, len, SAVE_STATE_TYPE_STATUS, SAVE_STATE_COOKIE_TYPE);
    *data = tox->m->userstatus;
    data += len;

    Node_format relays[NUM_SAVED_TCP_RELAYS];
    uint8_t *temp_data = data;
    data = save_write_subheader(temp_data, 0, SAVE_STATE_TYPE_TCP_RELAY, SAVE_STATE_COOKIE_TYPE);
    unsigned int num = copy_connected_tcp_relays(tox->net_crypto, relays, NUM_SAVED_TCP_RELAYS);
    int l = pack_nodes(data, NUM_SAVED_TCP_RELAYS * packed_node_size(TCP_INET6), relays, num);

    if (l > 0) {
        len = l;
        data = save_write_subheader(temp_data, len, SAVE_STATE_TYPE_TCP_RELAY, SAVE_STATE_COOKIE_TYPE);
        data += len;
    }

    return data;
}

int messenger_save_read_sections_callback(Tox *tox, const uint8_t *data, uint32_t length, uint16_t type)
{
    switch (type) {
        case SAVE_STATE_TYPE_OLDFRIENDS:
            oldfriends_list_load(tox->m, data, length);
            break;

        case SAVE_STATE_TYPE_FRIENDS:
            friends_list_load(tox, data, length);
            break;

        case SAVE_STATE_TYPE_NAME:
            if ((length > 0) && (length <= MAX_NAME_LENGTH)) {
                setname(tox->m, data, length);
            }

            break;

        case SAVE_STATE_TYPE_STATUSMESSAGE:
            if ((length > 0) && (length < MAX_STATUSMESSAGE_LENGTH)) {
                m_set_statusmessage(tox, data, length);
            }

            break;

        case SAVE_STATE_TYPE_STATUS:
            if (length == 1) {
                m_set_userstatus(tox, *data);
            }

            break;

        case SAVE_STATE_TYPE_TCP_RELAY: {
            if (length == 0) {
                break;
            }

            unpack_nodes(tox->m->loaded_relays, NUM_SAVED_TCP_RELAYS, 0, data, length, 1);
            tox->m->has_added_relays = 0;

            break;
        }
    }

    return 0;
}

/* Return the number of friends in the instance m.
 * You should use this to determine how much memory to allocate
 * for copy_friendlist. */
uint32_t count_friendlist(const Messenger *m)
{
    uint32_t ret = 0;
    uint32_t i;

    for (i = 0; i < m->numfriends; i++) {
        if (m->friendlist[i].status > 0) {
            ret++;
        }
    }

    return ret;
}

/* Copy a list of valid friend IDs into the array out_list.
 * If out_list is NULL, returns 0.
 * Otherwise, returns the number of elements copied.
 * If the array was too small, the contents
 * of out_list will be truncated to list_size. */
uint32_t copy_friendlist(Messenger const *m, uint32_t *out_list, uint32_t list_size)
{
    if (!out_list)
        return 0;

    if (m->numfriends == 0) {
        return 0;
    }

    uint32_t i;
    uint32_t ret = 0;

    for (i = 0; i < m->numfriends; i++) {
        if (ret >= list_size) {
            break; /* Abandon ship */
        }

        if (m->friendlist[i].status > 0) {
            out_list[ret] = i;
            ret++;
        }
    }

    return ret;
}
