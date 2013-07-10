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

#ifndef _KONFERENCE_MEMBER_H
#define _KONFERENCE_MEMBER_H

//
// includes
//

#include <stdio.h>

#include "app_conference.h"
#include "conference.h"

//
// defines
//

#define MEMBER_FLAGS_LEN 10
#define MEMBER_TYPE_LEN 20

//
// struct declarations
//

struct ast_conf_soundq
{
	char name[256];
	struct ast_filestream *stream; // the stream
	int stopped;
	struct ast_conf_soundq *next;
};

struct ast_conf_frameq
{
	ast_mutex_t lock;
	AST_LIST_HEAD_NOLOCK(, ast_frame) frames;
	unsigned int count;
};

struct ast_conf_member
{
	ast_mutex_t lock; // member data mutex

	struct ast_channel* chan; // member's channel

	ast_conference* conf; // member's conference

	ast_cond_t delete_var; // delete cv
	char delete_flag; // delete flag
	int use_count; // use count

	conf_frame *speaker_frame; // member speaker frame

	// values passed to create_member() via *data
	char flags[MEMBER_FLAGS_LEN + 1];	// raw member-type flags
	char type[MEMBER_TYPE_LEN + 1];		// conference type
	char *spyee_channel_name; // spyee  channel name

	int max_users; // zero or max users for this conference

	// sounds to play to the member when somebody joins/leaves the conference
	char * join_sound;
	char * leave_sound;

	// block ids
	int conf_id;
#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
	int score_id;
#endif

	// muting options - this member will not be heard/seen
	int mute_audio;
	int muted; // should incoming audio be muted while we play?
	// volume level adjustment for this member
	int talk_volume;
	int listen_volume;

	// this member will not hear/see
	int norecv_audio;
	// is this person a moderator?
	int ismoderator;
	int kick_conferees;
	int kick_flag;

	// ready flag
	short ready_for_outgoing;

	// input frame queue
	ast_conf_frameq incomingq;

	// output frame queue
	ast_conf_frameq outgoingq;

	// relay dtmf to manager?
	short dtmf_relay;

	// speaking flag
	short is_speaking;

	// pointer to next member in linked list
	ast_conf_member* next;

	// pointer to prev member in linked list
	ast_conf_member* prev;

	// pointer to member's bucket list head
	struct channel_bucket *bucket;
	// list entry for member's bucket list
	AST_LIST_ENTRY(ast_conf_member) hash_entry;

	// spyer pointer to spyee or vice versa
	ast_conf_member* spy_partner;
	// spyee pointer to whisper frame
	conf_frame* whisper_frame;

	// start time
	struct timeval time_entered;

#if	SILDET == 1
	// voice flags
	int via_telephone;
	// pointer to webrtc preprocessor dsp
	VadInst *dsp;
	// translator for dsp
	struct ast_trans_pvt *to_dsp;
        // number of "silent" frames to ignore
	int ignore_vad_result;

#elif	SILDET == 2
	// voice flags
	int via_telephone;
	int vad_flag;
	int denoise_flag;
	int agc_flag;

	// vad voice probability thresholds
	float vad_prob_start;
	float vad_prob_continue;

	// pointer to speex preprocessor dsp
	SpeexPreprocessState *dsp;
	// translator for dsp
	struct ast_trans_pvt *to_dsp;
        // number of "silent" frames to ignore
	int ignore_vad_result;
#endif

	// audio format this member is using
	int write_format_index;
	int read_format_index;

	// member frame translators
	struct ast_trans_pvt* to_slinear;
	struct ast_trans_pvt* from_slinear;

	// For playing sounds
	ast_conf_soundq *soundq;

	// speaker mix buffer
	char *speakerBuffer;

	// speaker mix frames
	struct ast_frame *mixAstFrame;
	conf_frame *mixConfFrame;

	FILE* dump_outgoing_frames_file;
	FILE* dump_incomming_frames_file;
};

//
// function declarations
//

#if	ASTERISK_SRC_VERSION == 104
int member_exec(struct ast_channel* chan, void* data);
#else
int member_exec(struct ast_channel* chan, const char* data);
#endif

void member_play_sound( struct ast_conf_member *, const char *) ;

ast_conf_member* create_member(struct ast_channel* chan, const char* data, char* conf_name);
ast_conf_member* delete_member(ast_conf_member* member);

// incoming queue
void queue_incoming_frame(ast_conf_member* member, struct ast_frame* fr);
conf_frame* get_incoming_frame(ast_conf_member* member);

// outgoing queue
void queue_outgoing_frame(ast_conf_member* member, struct ast_frame* fr, struct timeval delivery);
struct ast_frame* get_outgoing_frame(ast_conf_member* member);

void member_process_spoken_frames(ast_conference* conf,
				  ast_conf_member *member,
				  conf_frame **spoken_frames,
				 int *listener_count,
				 int *speaker_count);

void member_process_outgoing_frames(ast_conference* conf,
				    ast_conf_member *member);

#endif
