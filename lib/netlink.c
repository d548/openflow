/* Copyright (C) 2007 Board of Trustees, Leland Stanford Jr. University.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "netlink.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "buffer.h"
#include "util.h"

#include "vlog.h"
#define THIS_MODULE VLM_netlink

/* Linux header file confusion causes this to be undefined. */
#ifndef SOL_NETLINK
#define SOL_NETLINK 270
#endif

/* Netlink sockets. */

struct nl_sock
{
    int fd;
    uint32_t pid;
};

/* Next nlmsghdr sequence number.
 * 
 * This implementation uses sequence numbers that are unique process-wide, to
 * avoid a hypothetical race: send request, close socket, open new socket that
 * reuses the old socket's PID value, send request on new socket, receive reply
 * from kernel to old socket but with same PID and sequence number.  (This race
 * could be avoided other ways, e.g. by preventing PIDs from being quickly
 * reused). */
static uint32_t next_seq;

static int alloc_pid(uint32_t *);
static void free_pid(uint32_t);

/* Creates a new netlink socket for the given netlink 'protocol'
 * (NETLINK_ROUTE, NETLINK_GENERIC, ...).  Returns 0 and sets '*sockp' to the
 * new socket if successful, otherwise returns a positive errno value.
 *
 * If 'multicast_group' is nonzero, the new socket subscribes to the specified
 * netlink multicast group.  (A netlink socket may listen to an arbitrary
 * number of multicast groups, but so far we only need one at a time.)
 *
 * Nonzero 'so_sndbuf' or 'so_rcvbuf' override the kernel default send or
 * receive buffer size, respectively.
 */
int
nl_sock_create(int protocol, int multicast_group,
               size_t so_sndbuf, size_t so_rcvbuf, struct nl_sock **sockp)
{
    struct nl_sock *sock;
    struct sockaddr_nl local, remote;
    int retval = 0;

    if (next_seq == 0) {
        /* Pick initial sequence number. */
        next_seq = getpid() ^ time(0);
    }

    *sockp = NULL;
    sock = malloc(sizeof *sock);
    if (sock == NULL) {
        return ENOMEM;
    }

    sock->fd = socket(AF_NETLINK, SOCK_RAW, protocol);
    if (sock->fd < 0) {
        VLOG_ERR("fcntl: %s", strerror(errno));
        goto error;
    }

    retval = alloc_pid(&sock->pid);
    if (retval) {
        goto error;
    }

    if (so_sndbuf != 0
        && setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF,
                      &so_sndbuf, sizeof so_sndbuf) < 0) {
        VLOG_ERR("setsockopt(SO_SNDBUF,%zu): %s", so_sndbuf, strerror(errno));
        goto error_free_pid;
    }

    if (so_rcvbuf != 0
        && setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF,
                      &so_rcvbuf, sizeof so_rcvbuf) < 0) {
        VLOG_ERR("setsockopt(SO_RCVBUF,%zu): %s", so_rcvbuf, strerror(errno));
        goto error_free_pid;
    }

    /* Bind local address as our selected pid. */
    memset(&local, 0, sizeof local);
    local.nl_family = AF_NETLINK;
    local.nl_pid = sock->pid;
    if (multicast_group > 0 && multicast_group <= 32) {
        /* This method of joining multicast groups is supported by old kernels,
         * but it only allows 32 multicast groups per protocol. */
        local.nl_groups |= 1ul << (multicast_group - 1);
    }
    if (bind(sock->fd, (struct sockaddr *) &local, sizeof local) < 0) {
        VLOG_ERR("bind(%"PRIu32"): %s", sock->pid, strerror(errno));
        goto error_free_pid;
    }

    /* Bind remote address as the kernel (pid 0). */
    memset(&remote, 0, sizeof remote);
    remote.nl_family = AF_NETLINK;
    remote.nl_pid = 0;
    if (connect(sock->fd, (struct sockaddr *) &remote, sizeof remote) < 0) {
        VLOG_ERR("connect(0): %s", strerror(errno));
        goto error_free_pid;
    }

    /* This method of joining multicast groups is only supported by newish
     * kernels, but it allows for an arbitrary number of multicast groups. */
    if (multicast_group > 32
        && setsockopt(sock->fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
                      &multicast_group, sizeof multicast_group) < 0) {
        VLOG_ERR("setsockopt(NETLINK_ADD_MEMBERSHIP,%d): %s",
                 multicast_group, strerror(errno));
        goto error_free_pid;
    }

    *sockp = sock;
    return 0;

error_free_pid:
    free_pid(sock->pid);
error:
    if (retval == 0) {
        retval = errno;
        if (retval == 0) {
            retval = EINVAL;
        }
    }
    if (sock->fd >= 0) {
        close(sock->fd);
    }
    free(sock);
    return retval;
}

/* Destroys netlink socket 'sock'. */
void
nl_sock_destroy(struct nl_sock *sock) 
{
    if (sock) {
        close(sock->fd);
        free_pid(sock->pid);
        free(sock);
    }
}

/* Tries to send 'msg', which must contain a Netlink message, to the kernel on
 * 'sock'.  nlmsg_len in 'msg' will be finalized to match msg->size before the
 * message is sent.
 *
 * Returns 0 if successful, otherwise a positive errno value.  If
 * 'wait' is true, then the send will wait until buffer space is ready;
 * otherwise, returns EAGAIN if the 'sock' send buffer is full. */
int
nl_sock_send(struct nl_sock *sock, const struct buffer *msg, bool wait) 
{
    int retval;

    nl_msg_nlmsghdr(msg)->nlmsg_len = msg->size;
    do {
        retval = send(sock->fd, msg->data, msg->size, wait ? 0 : MSG_DONTWAIT);
    } while (retval < 0 && errno == EINTR);
    return retval < 0 ? errno : 0;
}

/* Tries to send the 'n_iov' chunks of data in 'iov' to the kernel on 'sock' as
 * a single Netlink message.  (The message must be fully formed and not require
 * finalization of its nlmsg_len field.)
 *
 * Returns 0 if successful, otherwise a positive errno value.  If 'wait' is
 * true, then the send will wait until buffer space is ready; otherwise,
 * returns EAGAIN if the 'sock' send buffer is full. */
int
nl_sock_sendv(struct nl_sock *sock, const struct iovec iov[], size_t n_iov,
              bool wait) 
{
    struct msghdr msg;
    int retval;

    memset(&msg, 0, sizeof msg);
    msg.msg_iov = (struct iovec *) iov;
    msg.msg_iovlen = n_iov;
    do {
        retval = sendmsg(sock->fd, &msg, MSG_DONTWAIT);
    } while (retval < 0 && errno == EINTR);
    return retval < 0 ? errno : 0;
}

/* Tries to receive a netlink message from the kernel on 'sock'.  If
 * successful, stores the received message into '*bufp' and returns 0.  The
 * caller is responsible for destroying the message with buffer_delete().  On
 * failure, returns a positive errno value and stores a null pointer into
 * '*bufp'.
 *
 * If 'wait' is true, nl_sock_recv waits for a message to be ready; otherwise,
 * returns EAGAIN if the 'sock' receive buffer is empty. */
int
nl_sock_recv(struct nl_sock *sock, struct buffer **bufp, bool wait) 
{
    uint8_t tmp;
    ssize_t bufsize = 2048;
    ssize_t nbytes, nbytes2;
    struct buffer *buf;
    struct nlmsghdr *nlmsghdr;
    struct iovec iov;
    struct msghdr msg = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = NULL,
        .msg_controllen = 0,
        .msg_flags = 0
    };

    buf = buffer_new(bufsize);
    *bufp = NULL;

try_again:
    /* Attempt to read the message.  We don't know the size of the data
     * yet, so we take a guess at 2048.  If we're wrong, we keep trying
     * and doubling the buffer size each time. 
     */
    nlmsghdr = buffer_put_uninit(buf, bufsize);
    iov.iov_base = nlmsghdr;
    iov.iov_len = bufsize;
    do {
        nbytes = recvmsg(sock->fd, &msg, (wait ? 0 : MSG_DONTWAIT) | MSG_PEEK); 
    } while (nbytes < 0 && errno == EINTR);
    if (nbytes < 0) {
        buffer_delete(buf);
        return errno;
    }
    if (msg.msg_flags & MSG_TRUNC) {
        bufsize *= 2;
        buffer_reinit(buf, bufsize);
        goto try_again;
    }
    buf->size = nbytes;

    /* We successfully read the message, so recv again to clear the queue */
    iov.iov_base = &tmp;
    iov.iov_len = 1;
    do {
        nbytes2 = recvmsg(sock->fd, &msg, MSG_DONTWAIT);
        if (nbytes2 < 0) {
            VLOG_ERR("failed to remove nlmsg from socket: %d\n", errno);
        }
    } while (nbytes2 < 0 && errno == EINTR);

    if (!NLMSG_OK(nlmsghdr, nbytes)) {
        VLOG_ERR("received invalid nlmsg (%zd bytes < %d)",
                 bufsize, NLMSG_HDRLEN);
        buffer_delete(buf);
        return EPROTO;
    }
    *bufp = buf;
    return 0;
}

/* Sends 'request' to the kernel via 'sock' and waits for a response.  If
 * successful, stores the reply into '*replyp' and returns 0.  The caller is
 * responsible for destroying the reply with buffer_delete().  On failure,
 * returns a positive errno value and stores a null pointer into '*replyp'.
 *
 * Bare Netlink is an unreliable transport protocol.  This function layers
 * reliable delivery and reply semantics on top of bare Netlink.
 * 
 * In Netlink, sending a request to the kernel is reliable enough, because the
 * kernel will tell us if the message cannot be queued (and we will in that
 * case put it on the transmit queue and wait until it can be delivered).
 * 
 * Receiving the reply is the real problem: if the socket buffer is full when
 * the kernel tries to send the reply, the reply will be dropped.  However, the
 * kernel sets a flag that a reply has been dropped.  The next call to recv
 * then returns ENOBUFS.  We can then re-send the request.
 *
 * Caveats:
 *
 *      1. Netlink depends on sequence numbers to match up requests and
 *         replies.  The sender of a request supplies a sequence number, and
 *         the reply echos back that sequence number.
 *
 *         This is fine, but (1) some kernel netlink implementations are
 *         broken, in that they fail to echo sequence numbers and (2) this
 *         function will drop packets with non-matching sequence numbers, so
 *         that only a single request can be usefully transacted at a time.
 *
 *      2. Resending the request causes it to be re-executed, so the request
 *         needs to be idempotent.
 */
int
nl_sock_transact(struct nl_sock *sock,
                 const struct buffer *request, struct buffer **replyp) 
{
    uint32_t seq = nl_msg_nlmsghdr(request)->nlmsg_seq;
    struct nlmsghdr *nlmsghdr;
    struct buffer *reply;
    int retval;

    *replyp = NULL;

    /* Ensure that we get a reply even if this message doesn't ordinarily call
     * for one. */
    nl_msg_nlmsghdr(request)->nlmsg_flags |= NLM_F_ACK;
    
send:
    retval = nl_sock_send(sock, request, true);
    if (retval) {
        return retval;
    }

recv:
    retval = nl_sock_recv(sock, &reply, true);
    if (retval) {
        if (retval == ENOBUFS) {
            VLOG_DBG("receive buffer overflow, resending request");
            goto send;
        } else {
            return retval;
        }
    }
    nlmsghdr = nl_msg_nlmsghdr(reply);
    if (seq != nlmsghdr->nlmsg_seq) {
        VLOG_DBG("ignoring seq %"PRIu32" != expected %"PRIu32,
                 nl_msg_nlmsghdr(reply)->nlmsg_seq, seq);
        buffer_delete(reply);
        goto recv;
    }
    if (nl_msg_nlmsgerr(reply, &retval)) {
        if (retval) {
            VLOG_DBG("received NAK error=%d (%s)", retval, strerror(retval));
        }
        return retval != EAGAIN ? retval : EPROTO;
    }

    *replyp = reply;
    return 0;
}

/* Returns 'sock''s underlying file descriptor. */
int
nl_sock_fd(const struct nl_sock *sock) 
{
    return sock->fd;
}

/* Netlink messages. */

/* Returns the nlmsghdr at the head of 'msg'.
 *
 * 'msg' must be at least as large as a nlmsghdr. */
struct nlmsghdr *
nl_msg_nlmsghdr(const struct buffer *msg) 
{
    return buffer_at_assert(msg, 0, NLMSG_HDRLEN);
}

/* Returns the genlmsghdr just past 'msg''s nlmsghdr.
 *
 * Returns a null pointer if 'msg' is not large enough to contain an nlmsghdr
 * and a genlmsghdr. */
struct genlmsghdr *
nl_msg_genlmsghdr(const struct buffer *msg) 
{
    return buffer_at(msg, NLMSG_HDRLEN, GENL_HDRLEN);
}

/* If 'buffer' is a NLMSG_ERROR message, stores 0 in '*errorp' if it is an ACK
 * message, otherwise a positive errno value, and returns true.  If 'buffer' is
 * not an NLMSG_ERROR message, returns false.
 *
 * 'msg' must be at least as large as a nlmsghdr. */
bool
nl_msg_nlmsgerr(const struct buffer *msg, int *errorp) 
{
    if (nl_msg_nlmsghdr(msg)->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = buffer_at(msg, NLMSG_HDRLEN, sizeof *err);
        int code = EPROTO;
        if (!err) {
            VLOG_ERR("received invalid nlmsgerr (%zd bytes < %zd)",
                     msg->size, NLMSG_HDRLEN + sizeof *err);
        } else if (err->error <= 0 && err->error > INT_MIN) {
            code = -err->error;
        }
        if (errorp) {
            *errorp = code;
        }
        return true;
    } else {
        return false;
    }
}

/* Ensures that 'b' has room for at least 'size' bytes plus netlink pading at
 * its tail end, reallocating and copying its data if necessary. */
void
nl_msg_reserve(struct buffer *msg, size_t size) 
{
    buffer_reserve_tailroom(msg, NLMSG_ALIGN(size));
}

/* Puts a nlmsghdr at the beginning of 'msg', which must be initially empty.
 * Uses the given 'type' and 'flags'.  'sock' is used to obtain a PID and
 * sequence number for proper routing of replies.  'expected_payload' should be
 * an estimate of the number of payload bytes to be supplied; if the size of
 * the payload is unknown a value of 0 is acceptable.
 *
 * 'type' is ordinarily an enumerated value specific to the Netlink protocol
 * (e.g. RTM_NEWLINK, for NETLINK_ROUTE protocol).  For Generic Netlink, 'type'
 * is the family number obtained via nl_lookup_genl_family().
 *
 * 'flags' is a bit-mask that indicates what kind of request is being made.  It
 * is often NLM_F_REQUEST indicating that a request is being made, commonly
 * or'd with NLM_F_ACK to request an acknowledgement.
 *
 * nl_msg_put_genlmsghdr is more convenient for composing a Generic Netlink
 * message. */
void
nl_msg_put_nlmsghdr(struct buffer *msg, struct nl_sock *sock,
                    size_t expected_payload, uint32_t type, uint32_t flags) 
{
    struct nlmsghdr *nlmsghdr;

    assert(msg->size == 0);

    nl_msg_reserve(msg, NLMSG_HDRLEN + expected_payload);
    nlmsghdr = nl_msg_put_uninit(msg, NLMSG_HDRLEN);
    nlmsghdr->nlmsg_len = 0;
    nlmsghdr->nlmsg_type = type;
    nlmsghdr->nlmsg_flags = flags;
    nlmsghdr->nlmsg_seq = ++next_seq;
    nlmsghdr->nlmsg_pid = sock->pid;
}

/* Puts a nlmsghdr and genlmsghdr at the beginning of 'msg', which must be
 * initially empty.  'sock' is used to obtain a PID and sequence number for
 * proper routing of replies.  'expected_payload' should be an estimate of the
 * number of payload bytes to be supplied; if the size of the payload is
 * unknown a value of 0 is acceptable.
 *
 * 'family' is the family number obtained via nl_lookup_genl_family().
 *
 * 'flags' is a bit-mask that indicates what kind of request is being made.  It
 * is often NLM_F_REQUEST indicating that a request is being made, commonly
 * or'd with NLM_F_ACK to request an acknowledgement.
 *
 * 'cmd' is an enumerated value specific to the Generic Netlink family
 * (e.g. CTRL_CMD_NEWFAMILY for the GENL_ID_CTRL family).
 *
 * 'version' is a version number specific to the family and command (often 1).
 *
 * nl_msg_put_nlmsghdr should be used to compose Netlink messages that are not
 * Generic Netlink messages. */
void
nl_msg_put_genlmsghdr(struct buffer *msg, struct nl_sock *sock,
                      size_t expected_payload, int family, uint32_t flags,
                      uint8_t cmd, uint8_t version)
{
    struct genlmsghdr *genlmsghdr;

    nl_msg_put_nlmsghdr(msg, sock, GENL_HDRLEN + expected_payload,
                        family, flags);
    assert(msg->size == NLMSG_HDRLEN);
    genlmsghdr = nl_msg_put_uninit(msg, GENL_HDRLEN);
    genlmsghdr->cmd = cmd;
    genlmsghdr->version = version;
    genlmsghdr->reserved = 0;
}

/* Appends the 'size' bytes of data in 'p', plus Netlink padding if needed, to
 * the tail end of 'msg'.  Data in 'msg' is reallocated and copied if
 * necessary. */
void
nl_msg_put(struct buffer *msg, const void *data, size_t size) 
{
    memcpy(nl_msg_put_uninit(msg, size), data, size);
}

/* Appends 'size' bytes of data, plus Netlink padding if needed, to the tail
 * end of 'msg', reallocating and copying its data if necessary.  Returns a
 * pointer to the first byte of the new data, which is left uninitialized. */
void *
nl_msg_put_uninit(struct buffer *msg, size_t size) 
{
    size_t pad = NLMSG_ALIGN(size) - size;
    char *p = buffer_put_uninit(msg, size + pad);
    if (pad) {
        memset(p + size, 0, pad); 
    }
    return p;
}

/* Appends a Netlink attribute of the given 'type' and room for 'size' bytes of
 * data as its payload, plus Netlink padding if needed, to the tail end of
 * 'msg', reallocating and copying its data if necessary.  Returns a pointer to
 * the first byte of data in the attribute, which is left uninitialized. */
void *
nl_msg_put_unspec_uninit(struct buffer *msg, uint16_t type, size_t size) 
{
    size_t total_size = NLA_HDRLEN + size;
    struct nlattr* nla = nl_msg_put_uninit(msg, total_size);
    assert(NLA_ALIGN(total_size) <= UINT16_MAX);
    nla->nla_len = total_size;
    nla->nla_type = type;
    return nla + 1;
}

/* Appends a Netlink attribute of the given 'type' and the 'size' bytes of
 * 'data' as its payload, to the tail end of 'msg', reallocating and copying
 * its data if necessary.  Returns a pointer to the first byte of data in the
 * attribute, which is left uninitialized. */
void
nl_msg_put_unspec(struct buffer *msg, uint16_t type,
                  const void *data, size_t size) 
{
    memcpy(nl_msg_put_unspec_uninit(msg, type, size), data, size);
}

/* Appends a Netlink attribute of the given 'type' and no payload to 'msg'.
 * (Some Netlink protocols use the presence or absence of an attribute as a
 * Boolean flag.) */
void
nl_msg_put_flag(struct buffer *msg, uint16_t type) 
{
    nl_msg_put_unspec(msg, type, NULL, 0);
}

/* Appends a Netlink attribute of the given 'type' and the given 8-bit 'value'
 * to 'msg'. */
void
nl_msg_put_u8(struct buffer *msg, uint16_t type, uint8_t value) 
{
    nl_msg_put_unspec(msg, type, &value, sizeof value);
}

/* Appends a Netlink attribute of the given 'type' and the given 16-bit 'value'
 * to 'msg'. */
void
nl_msg_put_u16(struct buffer *msg, uint16_t type, uint16_t value)
{
    nl_msg_put_unspec(msg, type, &value, sizeof value);
}

/* Appends a Netlink attribute of the given 'type' and the given 32-bit 'value'
 * to 'msg'. */
void
nl_msg_put_u32(struct buffer *msg, uint16_t type, uint32_t value)
{
    nl_msg_put_unspec(msg, type, &value, sizeof value);
}

/* Appends a Netlink attribute of the given 'type' and the given 64-bit 'value'
 * to 'msg'. */
void
nl_msg_put_u64(struct buffer *msg, uint16_t type, uint64_t value)
{
    nl_msg_put_unspec(msg, type, &value, sizeof value);
}

/* Appends a Netlink attribute of the given 'type' and the given
 * null-terminated string 'value' to 'msg'. */
void
nl_msg_put_string(struct buffer *msg, uint16_t type, const char *value)
{
    nl_msg_put_unspec(msg, type, value, strlen(value) + 1);
}

/* Appends a Netlink attribute of the given 'type' and the given buffered
 * netlink message in 'nested_msg' to 'msg'.  The nlmsg_len field in
 * 'nested_msg' is finalized to match 'nested_msg->size'. */
void
nl_msg_put_nested(struct buffer *msg,
                  uint16_t type, struct buffer *nested_msg)
{
    nl_msg_nlmsghdr(nested_msg)->nlmsg_len = nested_msg->size;
    nl_msg_put_unspec(msg, type, nested_msg->data, nested_msg->size);
}

/* Returns the first byte in the payload of attribute 'nla'. */
const void *
nl_attr_get(const struct nlattr *nla) 
{
    assert(nla->nla_len >= NLA_HDRLEN);
    return nla + 1;
}

/* Returns the number of bytes in the payload of attribute 'nla'. */
size_t
nl_attr_get_size(const struct nlattr *nla) 
{
    assert(nla->nla_len >= NLA_HDRLEN);
    return nla->nla_len - NLA_HDRLEN;
}

/* Asserts that 'nla''s payload is at least 'size' bytes long, and returns the
 * first byte of the payload. */
const void *
nl_attr_get_unspec(const struct nlattr *nla, size_t size) 
{
    assert(nla->nla_len >= NLA_HDRLEN + size);
    return nla + 1;
}

/* Returns true if 'nla' is nonnull.  (Some Netlink protocols use the presence
 * or absence of an attribute as a Boolean flag.) */
bool
nl_attr_get_flag(const struct nlattr *nla) 
{
    return nla != NULL;
}

#define NL_ATTR_GET_AS(NLA, TYPE) \
        (*(TYPE*) nl_attr_get_unspec(nla, sizeof(TYPE)))

/* Returns the 8-bit value in 'nla''s payload.
 *
 * Asserts that 'nla''s payload is at least 1 byte long. */
uint8_t
nl_attr_get_u8(const struct nlattr *nla) 
{
    return NL_ATTR_GET_AS(nla, uint8_t);
}

/* Returns the 16-bit value in 'nla''s payload.
 *
 * Asserts that 'nla''s payload is at least 2 bytes long. */
uint16_t
nl_attr_get_u16(const struct nlattr *nla) 
{
    return NL_ATTR_GET_AS(nla, uint16_t);
}

/* Returns the 32-bit value in 'nla''s payload.
 *
 * Asserts that 'nla''s payload is at least 4 bytes long. */
uint32_t
nl_attr_get_u32(const struct nlattr *nla) 
{
    return NL_ATTR_GET_AS(nla, uint32_t);
}

/* Returns the 64-bit value in 'nla''s payload.
 *
 * Asserts that 'nla''s payload is at least 8 bytes long. */
uint64_t
nl_attr_get_u64(const struct nlattr *nla) 
{
    return NL_ATTR_GET_AS(nla, uint64_t);
}

/* Returns the null-terminated string value in 'nla''s payload.
 *
 * Asserts that 'nla''s payload contains a null-terminated string. */
const char *
nl_attr_get_string(const struct nlattr *nla) 
{
    assert(nla->nla_len > NLA_HDRLEN);
    assert(memchr(nl_attr_get(nla), '\0', nla->nla_len - NLA_HDRLEN) != NULL);
    return nl_attr_get(nla);
}

/* Default minimum and maximum payload sizes for each type of attribute. */
static const size_t attr_len_range[][2] = {
    [0 ... N_NL_ATTR_TYPES - 1] = { 0, SIZE_MAX },
    [NL_A_U8] = { 1, 1 },
    [NL_A_U16] = { 2, 2 },
    [NL_A_U32] = { 4, 4 },
    [NL_A_U64] = { 8, 8 },
    [NL_A_STRING] = { 1, SIZE_MAX },
    [NL_A_FLAG] = { 0, SIZE_MAX },
    [NL_A_NESTED] = { NLMSG_HDRLEN, SIZE_MAX },
};

/* Parses the Generic Netlink payload of 'msg' as a sequence of Netlink
 * attributes.  'policy[i]', for 0 <= i < n_attrs, specifies how the attribute
 * with nla_type == i is parsed; a pointer to attribute i is stored in
 * attrs[i].  Returns true if successful, false on failure. */
bool
nl_policy_parse(const struct buffer *msg, const struct nl_policy policy[],
                struct nlattr *attrs[], size_t n_attrs)
{
    void *p, *tail;
    size_t n_required;
    size_t i;

    n_required = 0;
    for (i = 0; i < n_attrs; i++) {
        attrs[i] = NULL;

        assert(policy[i].type < N_NL_ATTR_TYPES);
        if (policy[i].type != NL_A_NO_ATTR
            && policy[i].type != NL_A_FLAG
            && !policy[i].optional) {
            n_required++;
        }
    }

    p = buffer_at(msg, NLMSG_HDRLEN + GENL_HDRLEN, 0);
    if (p == NULL) {
        VLOG_DBG("missing headers in nl_policy_parse");
        return false;
    }
    tail = buffer_tail(msg);

    while (p < tail) {
        size_t offset = p - msg->data;
        struct nlattr *nla = p;
        size_t len, aligned_len;
        uint16_t type;

        /* Make sure its claimed length is plausible. */
        if (nla->nla_len < NLA_HDRLEN) {
            VLOG_DBG("%zu: attr shorter than NLA_HDRLEN (%"PRIu16")",
                     offset, nla->nla_len);
            return false;
        }
        len = nla->nla_len - NLA_HDRLEN;
        aligned_len = NLA_ALIGN(len);
        if (aligned_len > tail - p) {
            VLOG_DBG("%zu: attr %"PRIu16" aligned data len (%zu) "
                     "> bytes left (%tu)",
                     offset, nla->nla_type, aligned_len, tail - p);
            return false;
        }

        type = nla->nla_type;
        if (type < n_attrs && policy[type].type != NL_A_NO_ATTR) {
            const struct nl_policy *p = &policy[type];
            size_t min_len, max_len;

            /* Validate length and content. */
            min_len = p->min_len ? p->min_len : attr_len_range[p->type][0];
            max_len = p->max_len ? p->max_len : attr_len_range[p->type][1];
            if (len < min_len || len > max_len) {
                VLOG_DBG("%zu: attr %"PRIu16" length %zu not in allowed range "
                         "%zu...%zu", offset, type, len, min_len, max_len);
                return false;
            }
            if (p->type == NL_A_STRING) {
                if (((char *) nla)[nla->nla_len - 1]) {
                    VLOG_DBG("%zu: attr %"PRIu16" lacks null terminator",
                             offset, type);
                    return false;
                }
                if (memchr(nla + 1, '\0', len - 1) != NULL) {
                    VLOG_DBG("%zu: attr %"PRIu16" lies about string length",
                             offset, type);
                    return false;
                }
            }
            if (!p->optional && attrs[type] == NULL) {
                assert(n_required > 0);
                --n_required;
            }
            attrs[type] = nla;
        } else {
            /* Skip attribute type that we don't care about. */
        }
        p += NLA_ALIGN(nla->nla_len);
    }
    if (n_required) {
        VLOG_DBG("%zu required attrs missing", n_required);
        return false;
    }
    return true;
}

/* Miscellaneous.  */

static const struct nl_policy family_policy[CTRL_ATTR_MAX + 1] = { 
    [CTRL_ATTR_FAMILY_ID] = {.type = NL_A_U16},
};

static int do_lookup_genl_family(const char *name) 
{
    struct nl_sock *sock;
    struct buffer request, *reply;
    struct nlattr *attrs[ARRAY_SIZE(family_policy)];
    int retval;

    retval = nl_sock_create(NETLINK_GENERIC, 0, 0, 0, &sock);
    if (retval) {
        return -retval;
    }

    buffer_init(&request, 0);
    nl_msg_put_genlmsghdr(&request, sock, 0, GENL_ID_CTRL, NLM_F_REQUEST,
                          CTRL_CMD_GETFAMILY, 1);
    nl_msg_put_string(&request, CTRL_ATTR_FAMILY_NAME, name);
    retval = nl_sock_transact(sock, &request, &reply);
    buffer_uninit(&request);
    if (retval) {
        nl_sock_destroy(sock);
        return -retval;
    }

    if (!nl_policy_parse(reply, family_policy, attrs,
                         ARRAY_SIZE(family_policy))) {
        nl_sock_destroy(sock);
        buffer_delete(reply);
        return -EPROTO;
    }

    retval = nl_attr_get_u16(attrs[CTRL_ATTR_FAMILY_ID]);
    if (retval == 0) {
        retval = -EPROTO;
    }
    nl_sock_destroy(sock);
    buffer_delete(reply);
    return retval;
}

/* If '*number' is 0, translates the given Generic Netlink family 'name' to a
 * number and stores it in '*number'.  If successful, returns 0 and the caller
 * may use '*number' as the family number.  On failure, returns a positive
 * errno value and '*number' caches the errno value. */
int
nl_lookup_genl_family(const char *name, int *number) 
{
    if (*number == 0) {
        *number = do_lookup_genl_family(name);
        assert(*number != 0);
    }
    return *number > 0 ? 0 : -*number;
}

/* Netlink PID.
 *
 * Every Netlink socket must be bound to a unique 32-bit PID.  By convention,
 * programs that have a single Netlink socket use their Unix process ID as PID,
 * and programs with multiple Netlink sockets add a unique per-socket
 * identifier in the bits above the Unix process ID.
 *
 * The kernel has Netlink PID 0.
 */

/* Parameters for how many bits in the PID should come from the Unix process ID
 * and how many unique per-socket. */
#define SOCKET_BITS 10
#define MAX_SOCKETS (1u << SOCKET_BITS)

#define PROCESS_BITS (32 - SOCKET_BITS)
#define MAX_PROCESSES (1u << PROCESS_BITS)
#define PROCESS_MASK ((uint32_t) (MAX_PROCESSES - 1))

/* Bit vector of unused socket identifiers. */
static uint32_t avail_sockets[ROUND_UP(MAX_SOCKETS, 32)];

/* Allocates and returns a new Netlink PID. */
static int
alloc_pid(uint32_t *pid)
{
    int i;

    for (i = 0; i < MAX_SOCKETS; i++) {
        if ((avail_sockets[i / 32] & (1u << (i % 32))) == 0) {
            avail_sockets[i / 32] |= 1u << (i % 32);
            *pid = (getpid() & PROCESS_MASK) | (i << PROCESS_BITS);
            return 0;
        }
    }
    VLOG_ERR("netlink pid space exhausted");
    return ENOBUFS;
}

/* Makes the specified 'pid' available for reuse. */
static void
free_pid(uint32_t pid)
{
    int sock = pid >> PROCESS_BITS;
    assert(avail_sockets[sock / 32] & (1u << (sock % 32)));
    avail_sockets[sock / 32] &= ~(1u << (sock % 32));
}