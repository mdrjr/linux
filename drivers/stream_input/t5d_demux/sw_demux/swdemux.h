/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef _SWDEMUX_H
#define _SWDEMUX_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USERSPACE
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define min(a,b) ((a) <= (b) ? (a) : (b))
#define max(a,b) ((a) >= (b) ? (a) : (b))
#endif

/**Boolean value true.*/
#define SWDMX_TRUE  1
/**Boolean value false.*/
#define SWDMX_FALSE 0

/**Function result: OK.*/
#define SWDMX_OK   1
/**Function result: no error but do nothing.*/
#define SWDMX_NONE 0
/**Function result: error.*/
#define SWDMX_ERR -1

/**
 * Allocate a new memory buffer.
 * \param size The size of the buffer.
 * \return The new buffer's pointer.
 */
static inline void *
swdmx_malloc(size_t size)
{
#ifdef USERSPACE
	return malloc(size);
#else
	return kmalloc(size, GFP_KERNEL);
#endif
}

/**
 * Resize a memory buffer's size.
 * \param optr The old buffer's pointer.
 * \param size The size of the new buffer.
 * \return The new buffer's pointer.
 */
static inline void *
swdmx_realloc(void *optr, size_t size)
{
#ifdef USERSPACE
	return realloc(optr, size);
#else
	return krealloc(optr, size, GFP_KERNEL);
#endif
}

/**
 * Free an unused buffer.
 * \param ptr The buffer to be freed.
 */
static inline void
swdmx_free(void *ptr)
{
#ifdef USERSPACE
	free(ptr);
#else
	kfree(ptr);
#endif
}


/**Section data callback function.*/
typedef void (*swdmx_sec_cb)(u8   *data,
			int     len,
			void      *udata);

/**Length of the section filter.*/
#define SWDMX_SEC_FILTER_LEN 16

/**TS packet filter's parameters.*/
struct swdmx_tsfilter_params {
	u16 pid; /**< PID of the stream.*/
};

/**Section filter's parameters.*/
struct swdmx_secfilter_params {
	u16 pid;                         /**< PID of the section.*/
	u8  crc32;                       /**< CRC32 check.*/
	u8  value[SWDMX_SEC_FILTER_LEN]; /**< Value array.*/
	u8  mask[SWDMX_SEC_FILTER_LEN];  /**< Mask array.*/
	u8  mode[SWDMX_SEC_FILTER_LEN];  /**< Match mode array.*/
};

/**TS packet.*/
struct swdmx_tspacket {
	u16  pid;           /**< PID.*/
	u8   payload_start; /**< Payload start flag.*/
	u8   priority;      /**< TS packet priority.*/
	u8   error;         /**< Error flag.*/
	u8   scramble;      /**< Scramble flag.*/
	u8   cc;            /**< Continuous counter.*/
	u8  *packet;        /**< Packet buffer.*/
	int  packet_len;    /**< Packet length.*/
	u8  *adp_field;     /**< Adaptation field buffer.*/
	int  adp_field_len; /**< Adaptation field length.*/
	u8  *payload;       /**< Payload buffer.*/
	int  payload_len;   /**< Payload length.*/
};

/**PES packet.*/
struct swdmx_pespacket {
	int  valid;        /**< The packet is valid.*/
	int  has_pts;      /**< Has PTS.*/
    u8   stream_id;    /**< stream id.*/
    u8   scramble;     /**< Scramble flag.*/
    u64  pts;          /**< PTS of the packet.*/
    u8  *data;         /**< Data buffer.*/
    u8  *payload;      /**< Payload buffer.*/
    int  data_len;     /**< Data length.*/
    int  data_cap;     /**< Capacity of the data buffer.*/
    int  payload_len;  /**< Payload length.*/
};

/**Descrambler key's parity.*/
enum swdmx_keyparity {
	SWDMX_DESC_ODD_KEY,  /**< Odd key.*/
	SWDMX_DESC_EVEN_KEY, /**< Even key.*/
	SWDMX_DESC_00_KEY,   /**< 00 key.*/
	SWDMX_DESC_ODD_IV,   /**< Odd key's IV data.*/
	SWDMX_DESC_EVEN_IV,  /**< Even key's ID data.*/
	SWDMX_DESC_00_IV     /**< 00 key's IV data.*/
};

/**Key's algorithm.*/
enum swdmx_keyalgo {
	SWDMX_DESC_ALGO_NONE,    /**< Not set.*/
	SWDMX_DESC_ALGO_AES_DEC, /**< AES decryption.*/
	SWDMX_DESC_ALGO_AES_ENC  /**< AES encryption.*/
};

/**Key entry.*/
struct swdmx_key {
	int   used;  /**< The key is used.*/
	int   is_iv; /**< The entry store the IV data.*/
	enum swdmx_keyalgo algo; /**< Key's algorithm.*/
	void *data;  /**< Key data.*/
	void (*set) (struct swdmx_key *key, const u8 *v, int len); /**< Set the key value.*/
	void (*free) (struct swdmx_key *key); /**< Free the key.*/
};

/**TS packet callback function.*/
typedef void (*swdmx_tspacket_cb)(struct swdmx_tspacket  *pkt,
			void        *udata);

/**PES packet callback function.*/
typedef void (*swdmx_pespacket_cb)(struct swdmx_pespacket *pkt,
            void *udata);

/**
 * Create a new TS packet parser.
 * \return The new TS parser.
 */
extern struct swdmx_ts_parser*
swdmx_ts_parser_new(void);

/**
 * Set the TS packet size.
 * \param tsp The TS parser.
 * \param size The packet size.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_ts_parser_set_packet_size(struct swdmx_ts_parser *tsp,
			int       size);

/**
 * Add a TS packet callback function to the TS parser.
 * \param tsp The TS parser.
 * \param cb The callback function.
 * \param data The user defined data used as the callback's parameter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_ts_parser_add_ts_packet_cb(struct swdmx_ts_parser   *tsp,
			swdmx_tspacket_cb  cb,
			void          *data);

/**
 * Remove a TS packet callback function from the TS parser.
 * \param tsp The TS parser.
 * \param cb The callback function.
 * \param data The user defined data used as the callback's parameter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_ts_parser_remove_ts_packet_cb(struct swdmx_ts_parser   *tsp,
			swdmx_tspacket_cb  cb,
			void          *data);

/**
 * Parse TS data.
 * \param tsp The TS parser.
 * \param data Input TS data.
 * \param len Input data length in bytes.
 * \return Parse data length in bytes.
 */
extern int
swdmx_ts_parser_run(struct swdmx_ts_parser *tsp,
			u8    *data,
			int       len);

/**
 * Free an unused TS parser.
 * \param tsp The TS parser to be freed.
 */
extern void
swdmx_ts_parser_free(struct swdmx_ts_parser *tsp);

/**
 * Create a new demux.
 * \return The new demux.
 */
extern struct swdmx_demux*
swdmx_demux_new(void);

/**
 * Allocate a new TS packet filter from the demux.
 * \param dmx The demux.
 * \return The new TS packet filter.
 */
extern struct swdmx_tsfilter*
swdmx_demux_alloc_ts_filter(struct swdmx_demux *dmx);

/**
 * Allocate a new section filter from the demux.
 * \param dmx The demux.
 * \return The new section filter.
 */
extern struct swdmx_secfilter*
swdmx_demux_alloc_sec_filter(struct swdmx_demux *dmx);

/**
 * TS packet input function of the demux.
 * \param pkt Input TS packet.
 * \param dmx The demux.
 */
extern void
swdmx_demux_ts_packet_cb(struct swdmx_tspacket *pkt,
			void        *dmx);

/**
 * Free an unused demux.
 * \param dmx The demux to be freed.
 */
extern void
swdmx_demux_free(struct swdmx_demux *dmx);

/**
 * Set the TS filter's parameters.
 * \param filter The filter.
 * \param params Parameters of the filter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_ts_filter_set_params(struct swdmx_tsfilter       *filter,
			struct swdmx_tsfilter_params *params);

/**
 * Add a TS packet callback to the TS filter.
 * \param filter The TS filter.
 * \param cb The callback function.
 * \param data User defined data used as callback's parameter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_ts_filter_add_ts_packet_cb(struct swdmx_tsfilter   *filter,
			swdmx_tspacket_cb  cb,
			void          *data);

/**
 * Remove a TS packet callback from the TS filter.
 * \param filter The TS filter.
 * \param cb The callback function.
 * \param data User defined data used as callback's parameter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_ts_filter_remove_ts_packet_cb(struct swdmx_tsfilter   *filter,
			swdmx_tspacket_cb  cb,
			void          *data);

/**
 * Enable the TS filter.
 * \param filter The TS filter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_ts_filter_enable(struct swdmx_tsfilter *filter);

/**
 * Disable the TS filter.
 * \param filter The filter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_ts_filter_disable(struct swdmx_tsfilter *filter);

/**
 * Free an unused TS filter.
 * \param filter The ts filter to be freed.
 */
extern void
swdmx_ts_filter_free(struct swdmx_tsfilter *filter);

/**
 * Set the section filter's parameters.
 * \param filter The section filter.
 * \param params Parameters of the filter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_sec_filter_set_params(struct swdmx_secfilter       *filter,
			struct swdmx_secfilter_params *params);

/**
 * Add a section callback to the section filter.
 * \param filter The section filter.
 * \param cb The callback function.
 * \param data User defined data used as callback's parameter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_sec_filter_add_section_cb(struct swdmx_secfilter *filter,
			swdmx_sec_cb      cb,
			void         *data);

/**
 * Remove a section callback from the section filter.
 * \param filter The section filter.
 * \param cb The callback function.
 * \param data User defined data used as callback's parameter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_sec_filter_remove_section_cb(struct swdmx_secfilter *filter,
			swdmx_sec_cb      cb,
			void         *data);

/**
 * Enable the section filter.
 * \param filter The section filter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_sec_filter_enable(struct swdmx_secfilter *filter);

/**
 * Disable the section filter.
 * \param filter The section filter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_sec_filter_disable(struct swdmx_secfilter *filter);

/**
 * Free an unused section filter.
 * \param filter The section filter to be freed.
 */
extern void
swdmx_sec_filter_free(struct swdmx_secfilter *filter);

/**
 * Create a new descrambler.
 * \return The new descrambler.
 */
extern struct swdmx_descrambler*
swdmx_descrambler_new (void);

/**
 * Descrambler TS packet callback.
 * @param pkt The TS packet.
 * @param data The descrambler.
 */
extern void
swdmx_descrambler_ts_packet_cb (
            struct swdmx_tspacket *pkt,
            void *data);

/**
 * Allocate a channel from the descrambler.
 * \param desc The descrambler.
 * \return The new channel.
 */
extern struct swdmx_descchannel*
swdmx_descrambler_alloc_channel (struct swdmx_descrambler *desc);

/**
 * Add a TS packet callback to the descrambler.
 * \param desc The descrambler.
 * \param cb The callback function.
 * \param data The user defined data used as callback's parameter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_descrambler_add_ts_packet_cb (
            struct swdmx_descrambler *desc,
            swdmx_tspacket_cb  cb,
            void              *data);

/**
 * Remove a TS packet callback from the descrambler.
 * \param desc The descrambler.
 * \param cb The callback function.
 * \param data The user defined data used as callback's parameter.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_descrambler_remove_ts_packet_cb (
            struct swdmx_descrambler *desc,
            swdmx_tspacket_cb   cb,
            void               *data);

/**
 * Free an unused descrambler.
 * \param desc The descrambler to be freed.
 */
extern void
swdmx_descrambler_free (struct swdmx_descrambler *desc);

/**
 * Create a new AES description algorithm.
 * @return The new algorithm,
 */
extern int
swdmx_aes_dec_key_init(struct swdmx_key *key);

/**
 * AES CBC decryption descrambling callback.
 * \param desc_chan The descrambler channel.
 * \param pkt The TS packet.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_aes_cbc_dec_desc_cb (struct swdmx_descchannel *desc_chan, struct swdmx_tspacket *pkt);

/**
 * Clear the key entry.
 * \param key The key entry.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_key_clear (struct swdmx_key *key);

/**
 * Set the key value.
 * \param key The key entry.
 * \param v The key value.
 * \param len The key's length.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_key_set (struct swdmx_key *key, const u8 *v, int len);

/**TS descrambling callback function.*/
typedef int (*swdmx_desc_cb) (struct swdmx_descchannel *desc_chan, struct swdmx_tspacket *pkt);

/**
 * Set the descrambler channel's descrambling callback.
 * \param chan The descrambler channel.
 * \param cb The descrambling callback.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_desc_channel_set_cb (
            struct swdmx_descchannel *chan,
            swdmx_desc_cb cb);

/**
 * Set the descrambler channel's related PID.
 * \param chan The descrambler channel.
 * \param pid The PID.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_desc_channel_set_pid (
            struct swdmx_descchannel *chan,
            u16       pid);

/**
 * Set the key of a descrambler channel.
 * \param chan The descrambler channel.
 * \param parity The key's parity.
 * \param key The key entry.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_desc_channel_set_key (
            struct swdmx_descchannel *chan,
			enum swdmx_keyparity parity,
            struct swdmx_key *key);

/**
 * Set the descrambler channel's SCB flag.
 * \param chan The descrambler channel.
 * \param as_is Keep the origin SCB.
 * \param scb The new SCB value.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_desc_channel_set_scb (
            struct swdmx_descchannel *chan,
            int as_is,
			int scb);

/**
 * Enable a descrambler channel.
 * \param chan The descrambler channel.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_desc_channel_enable (struct swdmx_descchannel *chan);

/**
 * Disable a descrambler channel.
 * \param chan The descrambler channel.
 * \retval SWDMX_OK On success.
 * \retval SWDMX_ERR On error.
 */
extern int
swdmx_desc_channel_disable (struct swdmx_descchannel *chan);

/**
 * Free an unused descrambler channel.
 * \param chan The descrambler channel to be freed.
 */
extern void
swdmx_desc_channel_free (struct swdmx_descchannel *chan);

/**
 * Create a new PES parser.
 * \param ops PES packet operation functions.
 * \param udata User defined parameter used in operation functions.
 * \return The new PES parser.
 */
extern struct swdmx_pesparser*
swdmx_pes_parser_new (void);

/**
 * Free an unused PES parser.
 * \param parser The PES parser to be freed.
 */
extern void
swdmx_pes_parser_free (struct swdmx_pesparser *parser);

/**
 * Add a PES packet callback function to the parser.
 * \param parser The PES parser.
 * \param cb The PES packet callback function.
 * \param data User defined parameter of callback.
 */
extern void
swdmx_pes_parser_add_pes_packet_cb (
            struct swdmx_pesparser   *parser,
            swdmx_pespacket_cb  cb,
            void *data);

/**
 * Remove a PES packet callback function from the parser.
 * \param parser The PES parser.
 * \param cb The PES packet callback function.
 * \param data User defined parameter of callback.
 */
extern void
swdmx_pes_parser_remove_pes_packet_cb (
            struct swdmx_pesparser   *parser,
            swdmx_pespacket_cb  cb,
            void *data);

/**
 * TS packet input function of the PES parser.
 * \param pkt Input TS packet.
 * \param parser The PES parser.
 */
extern void
swdmx_pes_parser_ts_packet_cb (
            struct swdmx_tspacket *pkt,
            void *parser);

#ifdef __cplusplus
}
#endif

#endif

