/*
 * app_konference
 *
 * A channel independent conference application for Asterisk
 *
 * Copyright (C) 2002, 2003 Junghanns.NET GmbH
 * Copyright (C) 2003, 2004 HorizonLive.com, Inc.
 * Copyright (C) 2005, 2005 Vipadia Limited
 * Copyright (C) 2005, 2006 HorizonWimba, Inc.
 * Copyright (C) 2007 Wimba, Inc.
 *
 * This program may be modified and distributed under the
 * terms of the GNU General Public License. You should have received
 * a copy of the GNU General Public License along with this
 * program; if not, write to the Free Software Foundation, Inc.
 * 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _KONFERENCE_APP_CONFERENCE_H
#define _KONFERENCE_APP_CONFERENCE_H


/* standard includes */
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
#include <sys/mman.h>
#endif

#include <pthread.h>

/* asterisk includes */
#include "asterisk.h"
#include <asterisk/utils.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/logger.h>
#include <asterisk/lock.h>
#include <asterisk/frame.h>
#include <asterisk/manager.h>
#include <asterisk/dsp.h>
#include <asterisk/translate.h>
#include <asterisk/channel.h>
#include <asterisk/file.h>
#include <asterisk/cli.h>
#include <asterisk/options.h>

#if	SILDET == 1
#include "libwebrtc/webrtc_vad.h"

// number of frames to ignore after speech detected
#define AST_CONF_FRAMES_TO_IGNORE 20

// modes: quality 0, low bitrate 1, aggressive 2, very aggressive 3
#define AST_CONF_VAD_MODE 0

#elif	 SILDET == 2
#include "libspeex/speex_preprocess.h"

// number of frames to ignore after speech detected
#define AST_CONF_FRAMES_TO_IGNORE 20

// our speex probability values
#define AST_CONF_PROB_START 0.05
#define AST_CONF_PROB_CONTINUE 0.02

#endif

#ifdef AC_USE_OPUS
#define AST_CONF_SAMPLE_RATE 48000
#define AST_FORMAT_CONFERENCE AST_FORMAT_SLINEAR48
#elif AC_USE_G722
#define AST_CONF_SAMPLE_RATE 16000
#define AST_FORMAT_CONFERENCE AST_FORMAT_SLINEAR16
#else
#define AST_CONF_SAMPLE_RATE 8000
#define AST_FORMAT_CONFERENCE AST_FORMAT_SLINEAR
#endif

#define AST_CONF_SAMPLE_SIZE 16
#define AST_CONF_BYTES_PER_SAMPLE (AST_CONF_SAMPLE_SIZE / 8)
#define AST_CONF_FRAME_INTERVAL 20

//
// so, since we cycle approximately every 20ms,
// we can compute the following values:
//
// 160 samples per 20 ms frame -or-
// (8000 samples-per-second * (20 ms / 1000 ms-per-second)) = 160 samples
//
// 320 bytes (2560 bits) of data  20 ms frame -or-
// (160 samples * 16 bits-per-sample / 8 bits-per-byte) = 320 bytes
//

#define AST_CONF_BLOCK_SAMPLES (AST_CONF_SAMPLE_RATE * AST_CONF_FRAME_INTERVAL / 1000)
#define AST_CONF_FRAME_DATA_SIZE (AST_CONF_BLOCK_SAMPLES * AST_CONF_BYTES_PER_SAMPLE)
#define AST_CONF_FRAMES_PER_SECOND (1000 / AST_CONF_FRAME_INTERVAL)

//
// buffer and queue values
//

// account for friendly offset when allocating buffer for frame
#define AST_CONF_BUFFER_SIZE (AST_CONF_FRAME_DATA_SIZE + AST_FRIENDLY_OFFSET)

// maximum number of frames queued per member
#define AST_CONF_MAX_QUEUE 100

//
// timer and sleep values
//

// milliseconds we're willing to wait for a channel
// event before we check for outgoing frames
#define AST_CONF_WAITFOR_LATENCY 40

//
// format translation values
//
	enum { 
		AC_CONF_INDEX = 0,
		AC_ULAW_INDEX,
		AC_ALAW_INDEX,
		AC_GSM_INDEX,
#ifdef	AC_USE_SPEEX
		AC_SPEEX_INDEX,
#endif
#ifdef	AC_USE_G729A
		AC_G729A_INDEX,
#endif
#ifdef	AC_USE_G722
		AC_SLINEAR_INDEX,
		AC_G722_INDEX,
#endif
#ifdef  AC_USE_OPUS
		AC_OPUS_INDEX,
#endif
		AC_SUPPORTED_FORMATS
		};

//
// Default conference max users is zero, that is, unbounded
//
#define AST_CONF_MAX_USERS 0

//
// Default conference type
//
#define AST_CONF_TYPE_DEFAULT "konference"

#define EVENT_FLAG_CONF EVENT_FLAG_USER

typedef struct ast_conference ast_conference;
typedef struct ast_conf_member ast_conf_member;
typedef struct ast_conf_soundq ast_conf_soundq;
typedef struct ast_conf_frameq ast_conf_frameq;
typedef struct conf_frame conf_frame;

const char *argument_delimiter;

#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
#define	SPEAKER_SCOREBOARD_FILE "/tmp/speaker-scoreboard"
char *speaker_scoreboard;
#endif

AST_LIST_HEAD(conference_bucket, ast_conference);
struct conference_bucket conference_table[CONFERENCE_TABLE_SIZE];

AST_LIST_HEAD(channel_bucket, ast_conf_member);
struct channel_bucket channel_table[CHANNEL_TABLE_SIZE];

#ifdef	CACHE_CONF_FRAMES
AST_LIST_HEAD_NOLOCK(confFrameList, conf_frame) confFrameList;
#endif

conf_frame *silent_conf_frame;

#ifdef	CACHE_CONTROL_BLOCKS
ast_conference *confblocklist;
ast_conf_member *mbrblocklist;
#endif

#if	ASTERISK_SRC_VERSION > 108
struct ast_format ast_format_conference;
struct ast_format ast_format_ulaw;
struct ast_format ast_format_alaw;
struct ast_format ast_format_gsm;
#ifdef	AC_USE_SPEEX
struct ast_format ast_format_speex;
#endif
#ifdef  AC_USE_G729A
struct ast_format ast_format_g729a;
#endif
#ifdef  AC_USE_G722
struct ast_format ast_format_slinear;
struct ast_format ast_format_g722;
#endif
#ifdef  AC_USE_OPUS
struct ast_format ast_format_opus;
#endif
#endif

#endif
