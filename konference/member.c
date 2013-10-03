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

#include <stdio.h>
#include "asterisk/autoconfig.h"
#include "asterisk/channel.h"
#include "member.h"
#include "frame.h"

#include "asterisk/musiconhold.h"

#ifdef	CACHE_CONTROL_BLOCKS
AST_MUTEX_DEFINE_STATIC(mbrblocklist_lock);

#ifdef	SPEAKER_SCOREBOARD
static int last_score_id;
AST_MUTEX_DEFINE_STATIC(speaker_scoreboard_lock);
#endif

#endif

// process an incoming frame.  Returns 0 normally, 1 if hangup was received.
static int process_incoming(ast_conf_member *member, ast_conference *conf, struct ast_frame *f)
{
	switch (f->frametype)
	{
		case AST_FRAME_VOICE:
		{
			if (member->mute_audio
				|| member->muted
				||  conf->membercount == 1)
			{
				// free the input frame
				ast_frfree(f);
				return 0;
			}
#if	SILDET == 1 || SILDET == 2
			// reset silence detection flag
			int is_silent_frame = 0;
			//
			// make sure we have a valid dsp and frame type
			//
			if (member->dsp)
			{
				// send the frame to the preprocessor
				f = convert_frame(member->to_dsp, f, 1);
				if (!f) {
					ast_log(LOG_ERROR, "convert_frame returned NULL frame\n");
					return 1;
				}

#if	SILDET == 1
//				ast_log(LOG_NOTICE, "sample rate for webRTC:  %d\n", AST_CONF_SAMPLE_RATE);

#if	ASTERISK_SRC_VERSION == 104
				if (!WebRtcVad_Process(member->dsp, AST_CONF_SAMPLE_RATE, f->data, AST_CONF_BLOCK_SAMPLES))
#else
				if (!WebRtcVad_Process(member->dsp, AST_CONF_SAMPLE_RATE, f->data.ptr, AST_CONF_BLOCK_SAMPLES))
#endif // ASTERISK_SRC_VERSION == 104

#elif	SILDET == 2
#if	ASTERISK_SRC_VERSION == 104
				if (!speex_preprocess(member->dsp, f->data, NULL))
#else
				if (!speex_preprocess(member->dsp, f->data.ptr, NULL))
#endif
#endif
				{
					//
					// we ignore the preprocessor's outcome if we've seen voice frames
					// in within the last AST_CONF_FRAMES_TO_SKIP frames
					//

					if (member->ignore_vad_result > 0)
					{
						// skip speex_preprocess(), and decrement counter
						if (!--member->ignore_vad_result) {
#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
							*(speaker_scoreboard + member->score_id) = '\x00';
#else
							char workspace[1024];
							char *varval = "<unknown>";
							get_unison_event_server_node_variable(member->chan, &varval, workspace, sizeof(workspace));

							manager_event(
								EVENT_FLAG_CONF,
								"ConferenceState",
								"Channel: %s\r\n"
								"UnisonEventServerNode: %s\r\n"
								"Flags: %s\r\n"
								"State: %s\r\n",
#if	ASTERISK_SRC_VERSION < 1100
								member->chan->name,
#else
								ast_channel_name(member->chan),
#endif
								S_OR(varval, ""),
								member->flags,
								"silent"
							);
#endif
						}
					}
					else
					{
						// set silent_frame flag
						is_silent_frame = 1;
					}
				}
				else
				{
					if (!member->ignore_vad_result) {
#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
						*(speaker_scoreboard + member->score_id) = '\x01';
#else
						char workspace[1024];
						char *varval = "<unknown>";
						get_unison_event_server_node_variable(member->chan, &varval, workspace, sizeof(workspace));

						manager_event(
							EVENT_FLAG_CONF,
							"ConferenceState",
							"Channel: %s\r\n"
							"UnisonEventServerNode: %s\r\n"
							"Flags: %s\r\n"
							"State: %s\r\n",
#if	ASTERISK_SRC_VERSION < 1100
							member->chan->name,
#else
							ast_channel_name(member->chan),
#endif
							S_OR(varval, ""),
							member->flags,
							"speaking"
						);
#endif
					}
					// voice detected, reset skip count
					member->ignore_vad_result = AST_CONF_FRAMES_TO_IGNORE;
				}
			}
			if (!is_silent_frame)
#endif
				queue_incoming_frame(member, f);
			break;
		}
		// In Asterisk 1.4 AST_FRAME_DTMF is equivalent to AST_FRAME_DTMF_END
		case AST_FRAME_DTMF:
		{
			if (member->dtmf_relay)
			{
				// output to manager...
				manager_event(
					EVENT_FLAG_CONF,
					"ConferenceDTMF",
					"ConferenceName: %s\r\n"
					"Type: %s\r\n"
					"UniqueID: %s\r\n"
					"Channel: %s\r\n"
					"CallerID: %s\r\n"
					"CallerIDName: %s\r\n"
					"Key: %c\r\n"
					"Count: %d\r\n"
					"Flags: %s\r\n"
					"Mute: %d\r\n",
					conf->name,
					member->type,
#if	ASTERISK_SRC_VERSION < 1100
					member->chan->uniqueid,
					member->chan->name,
#else
					ast_channel_uniqueid(member->chan),
					ast_channel_name(member->chan),
#endif
#if	ASTERISK_SRC_VERSION == 104 || ASTERISK_SRC_VERSION == 106
					member->chan->cid.cid_num ? member->chan->cid.cid_num : "unknown",
					member->chan->cid.cid_name ? member->chan->cid.cid_name : "unknown",
					f->subclass,
#else
#if	ASTERISK_SRC_VERSION < 1100
					member->chan->caller.id.number.str ? member->chan->caller.id.number.str : "unknown",
					member->chan->caller.id.name.str ? member->chan->caller.id.name.str: "unknown",
#else
					S_COR(ast_channel_caller(member->chan)->id.number.valid, ast_channel_caller(member->chan)->id.number.str, "<unknown>"),
					S_COR(ast_channel_caller(member->chan)->id.name.valid, ast_channel_caller(member->chan)->id.name.str, "<unknown>"),
#endif
					f->subclass.integer,
#endif
					conf->membercount,
					member->flags,
					member->mute_audio
					);

			}
			break;
		}
		case AST_FRAME_CONTROL:
		{
#if	ASTERISK_SRC_VERSION == 104 || ASTERISK_SRC_VERSION == 106
			if (f->subclass == AST_CONTROL_HANGUP)
#else
			if (f->subclass.integer == AST_CONTROL_HANGUP)
#endif
			{
				// hangup received

				// free the input frame
				ast_frfree(f);

				// break out of the while (42)
				return 1;
			}
			break;
		}
		case AST_FRAME_NULL:
		{
			if (member->kick_flag)
			{
				// free the input frame
				ast_frfree(f);

				// break out of the while (42)
				return 1;
			}
			break;
		}
		default:
		{
			break;
		}
	}

	// free the input frame
	ast_frfree(f);

	return 0;
}

// get the next frame from the soundq
static struct ast_frame *get_next_soundframe(ast_conf_member *member)
{
	struct ast_frame *f;

	while (!(f = member->soundq->stream && !member->soundq->stopped ? ast_readframe(member->soundq->stream) : NULL))
	{
		ast_conf_soundq *toboot = member->soundq;

		if (!toboot->stopped && !toboot->stream)
		{
#if	ASTERISK_SRC_VERSION < 1100
			if ((toboot->stream = ast_openstream(member->chan, toboot->name, member->chan->language)))
#else
			if ((toboot->stream = ast_openstream(member->chan, toboot->name, ast_channel_language(member->chan))))
#endif
				continue;
		}

		if (toboot->stream)
		{
			ast_stopstream(member->chan);
#ifdef	SOUND_COMPLETE_EVENTS
			// notify applications via mgr interface that this sound has been played
			manager_event(
				EVENT_FLAG_CONF,
				"ConferenceSoundComplete",
				"Channel: %s\r\n"
				"Sound: %s\r\n",
#if	ASTERISK_SRC_VERSION < 1100
				member->chan->name,
#else
				ast_channel_name(member->chan),
#endif
				toboot->name
			);
#endif
		}

		ast_mutex_lock(&member->lock);
		if (!(member->soundq = toboot->next))
		{
			member->muted = 0;
			ast_mutex_unlock(&member->lock);
			ast_free(toboot);
			return NULL;
		} else {
			ast_mutex_unlock(&member->lock);
			ast_free(toboot);
			continue;
		}

	}
	return f;
}


// process outgoing frames for the channel, playing either normal conference audio,
// or requested sounds
static void process_outgoing(ast_conf_member *member)
{
	struct ast_frame *cf,*sf;

	while ((cf = get_outgoing_frame(member)))
	{
		// if we're playing sounds, we can just replace the frame with the
		// next sound frame, and send it instead
		if (member->soundq)
		{
			if ((sf = get_next_soundframe(member)))
			{
				// use dequeued frame delivery time
				sf->delivery = cf->delivery;
		
				// free voice frame
				ast_frfree(cf);

				if (member->dump_outgoing_frames_file)
				{
					fwrite(sf->data.ptr, sf->datalen, 1, member->dump_outgoing_frames_file);
				}
				//ast_log(LOG_DEBUG, "write sound frame\n");
				// send sound frame
				ast_write(member->chan, sf);

				// free sound frame
				ast_frfree(sf);

				continue;
			}
		}

		if (member->dump_outgoing_frames_file)
		{
			fwrite(cf->data.ptr, cf->datalen, 1, member->dump_outgoing_frames_file);
		}

		//ast_log(LOG_DEBUG, "write voice frame\n");
		// send the frame
		ast_write(member->chan, cf);
		
		// free voice frame
		ast_frfree(cf);

	}
}

//
// main member thread function
//

#if	ASTERISK_SRC_VERSION == 104
int member_exec(struct ast_channel* chan, void* data)
#else
int member_exec(struct ast_channel* chan, const char* data)
#endif
{
	//
	// If the call has not yet been answered, answer the call
	// Note: asterisk apps seem to check _state, but it seems like it's safe
	// to just call ast_answer.  It will just do nothing if it is up.
	// it will also return -1 if the channel is a zombie, or has hung up.
	//

	if (ast_answer(chan))
	{
		ast_log(LOG_ERROR, "unable to answer call\n");
		return -1;
	}

	//
	// create a new member for the conference
 	//

	char conf_name[CONF_NAME_LEN + 1]  = { 0 };
	ast_conf_member *member;

	if (!(member = create_member(chan, (const char*)(data), conf_name)))
	{
		// unable to create member, return an error
		ast_log(LOG_ERROR, "unable to create member\n");
		return -1;
	}

	//
	// setup a conference for the new member
	//

	char max_users_flag = 0;
	ast_conference *conf;

	if (!(conf = join_conference(member, conf_name, &max_users_flag)))
	{
		ast_log(LOG_NOTICE, "unable to setup member conference %s: max_users_flag is %d\n", conf_name, max_users_flag);
		delete_member(member);
		return max_users_flag ? 0 : -1;
	}

	// add member to channel table
#if	ASTERISK_SRC_VERSION < 1100
	member->bucket = &(channel_table[hash(member->chan->name) % CHANNEL_TABLE_SIZE]);
#else
	member->bucket = &(channel_table[hash(ast_channel_name(member->chan)) % CHANNEL_TABLE_SIZE]);
#endif

	AST_LIST_LOCK(member->bucket);
	AST_LIST_INSERT_HEAD(member->bucket, member, hash_entry);
	AST_LIST_UNLOCK(member->bucket);

	char workspace[1024];
	char *varval = "<unknown>";
	get_unison_event_server_node_variable(member->chan, &varval, workspace, sizeof(workspace));

	manager_event(
		EVENT_FLAG_CONF,
		"ConferenceJoin",
		"ConferenceName: %s\r\n"
		"Conference UID: %s\r\n"
		"Type: %s\r\n"
		"UniqueID: %s\r\n"
		"Member: %d\r\n"
#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
		"ScoreID: %d\r\n"
#endif
		"Flags: %s\r\n"
		"Channel: %s\r\n"
		"CallerID: %s\r\n"
		"CallerIDName: %s\r\n"
		"UnisonEventServerNode: %s\r\n"
		"Moderators: %d\r\n"
		"Count: %d\r\n",
		conf->name,
		conf->conf_uid,
		member->type,
#if	ASTERISK_SRC_VERSION < 1100
		member->chan->uniqueid,
#else
		ast_channel_uniqueid(member->chan),
#endif
		member->conf_id,
#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
		member->score_id,
#endif
		member->flags,
#if	ASTERISK_SRC_VERSION < 1100
		member->chan->name,
#else
		ast_channel_name(member->chan),
#endif
#if	ASTERISK_SRC_VERSION == 104 || ASTERISK_SRC_VERSION == 106
		member->chan->cid.cid_num ? member->chan->cid.cid_num : "unknown",
		member->chan->cid.cid_name ? member->chan->cid.cid_name : "unknown",
#else
#if	ASTERISK_SRC_VERSION < 1100
		member->chan->caller.id.number.str ? member->chan->caller.id.number.str : "unknown",
		member->chan->caller.id.name.str ? member->chan->caller.id.name.str: "unknown",
#else
		S_COR(ast_channel_caller(member->chan)->id.number.valid, ast_channel_caller(member->chan)->id.number.str, "<unknown>"),
		S_COR(ast_channel_caller(member->chan)->id.name.valid, ast_channel_caller(member->chan)->id.name.str, "<unknown>"),
#endif
#endif
		S_OR(varval, ""),
		conf->moderators,
		conf->membercount
	);

	// if spyer setup failed, set variable and exit conference
	if (member->spyee_channel_name && !member->spy_partner)
	{
		remove_member(member, conf, conf_name);
		pbx_builtin_setvar_helper(member->chan, "KONFERENCE", "SPYFAILED");
		return 0;
	}

	// tell conference thread we're ready for frames
	member->ready_for_outgoing = 1;

	member->dump_outgoing_frames_file = 0;
	member->dump_incomming_frames_file = 0;
#ifdef DUMP_OUTGOING_FRAMES
	{
		char filename[1024];
		sprintf(filename, "/home/dzhukov/out-%s-%s",
		member->conf->conf_uid,
		S_COR(ast_channel_caller(member->chan)->id.number.valid, ast_channel_caller(member->chan)->id.number.str, "<unknown>"));
		member->dump_outgoing_frames_file = fopen(filename, "w+");
	}
#endif

	//
	// member thread loop
	//

	int left;
	struct ast_frame *f; // frame received from ast_read()
	int frame_count = 0;

	while (42)
	{
		// wait for an event on this channel
		if ((left  = ast_waitfor(chan, AST_CONF_WAITFOR_LATENCY)) > 0)
		{
			// a frame has come in before the latency timeout
			// was reached, so we process the frame

			if (!(f = ast_read(chan)) || process_incoming(member, conf, f))
			{
				// they probably want to hangup...
				break;
			}
#ifdef DEBUG_STATS
			if (f->frametype == AST_FRAME_VOICE && (++frame_count % 50 == 0))
			{
				ast_log(LOG_DEBUG,
						"Stats for member %s: frames read => %d\n",
						S_COR(ast_channel_caller(member->chan)->id.number.valid, ast_channel_caller(member->chan)->id.number.str, "<unknown>"),
						frame_count);
			}
#endif
		}
		else if (left == 0)
		{
			// no frame has arrived yet
#if	ASTERISK_SRC_VERSION < 1100
			// ast_log(LOG_NOTICE, "no frame available from channel, channel => %s\n", chan->name);
#else
			// ast_log(LOG_NOTICE, "no frame available from channel, channel => %s\n", ast_channel_name(chan));
#endif
		}
		else if (left < 0)
		{
			// an error occured
			ast_log(
				LOG_NOTICE,
				"an error occured waiting for a frame, channel => %s, error => %d\n",
#if	ASTERISK_SRC_VERSION < 1100
				chan->name, left
#else
				ast_channel_name(chan), left
#endif
			);
			break;
		}

		// process outgoing frames
		process_outgoing(member);
	}

	//
	// clean up
	//

#ifdef DUMP_OUTGOING_FRAMES
	if (member->dump_outgoing_frames_file)
	{
		fclose(member->dump_outgoing_frames_file);
		member->dump_outgoing_frames_file = 0;
	}
#endif

	if (member->kick_flag)
		pbx_builtin_setvar_helper(member->chan, "KONFERENCE", "KICKED");
	remove_member(member, conf, conf_name);
	return 0;
}

//
// manange member functions
//

ast_conf_member* create_member(struct ast_channel *chan, const char* data, char* conf_name)
{
	ast_conf_member *member;
#ifdef	CACHE_CONTROL_BLOCKS
#ifdef	SPEAKER_SCOREBOARD
	int score_id;
#endif
	if (mbrblocklist)
	{
		// get member control block from the free list
		ast_mutex_lock(&mbrblocklist_lock);
		member = mbrblocklist;
		mbrblocklist = mbrblocklist->next;
		ast_mutex_unlock(&mbrblocklist_lock);
#ifdef	SPEAKER_SCOREBOARD
		score_id = member->score_id;
#endif
		memset(member,0,sizeof(ast_conf_member));
	}
	else
	{
#endif
		// allocate new member control block
		if (!(member = ast_calloc(1,  sizeof(ast_conf_member))))
		{
			ast_log(LOG_ERROR, "unable to calloc ast_conf_member\n");
			return NULL;
		}
#ifdef	CACHE_CONTROL_BLOCKS
#ifdef	SPEAKER_SCOREBOARD
		ast_mutex_lock(&speaker_scoreboard_lock);
		score_id = last_score_id < SPEAKER_SCOREBOARD_SIZE ? ++last_score_id : 0;
		ast_mutex_unlock(&speaker_scoreboard_lock);
	}
	// initialize score board identifier
	member->score_id = score_id;
	// initialize score board entry
	*(speaker_scoreboard + score_id) = '\x00';
#else
	}
#endif
#endif

	// initialize mutexes
	ast_mutex_init(&member->lock);
	ast_mutex_init(&member->incomingq.lock);
	ast_mutex_init(&member->outgoingq.lock);

	// initialize cv
	ast_cond_init(&member->delete_var, NULL);

	// Default values for parameters that can get overwritten by dialplan arguments
#if	SILDET == 2
	member->vad_prob_start = AST_CONF_PROB_START;
	member->vad_prob_continue = AST_CONF_PROB_CONTINUE;
#endif
	member->max_users = AST_CONF_MAX_USERS;

	//
	// initialize member with passed data values
	//
	char argstr[256] = { 0 };

	// copy the passed data
	strncpy(argstr, data, sizeof(argstr) - 1);

	// point to the copied data
	char *stringp = argstr;

	// parse the id
	char *token;
	if ((token = strsep(&stringp, argument_delimiter)))
	{
		strncpy(conf_name, token, CONF_NAME_LEN);
	}
	else
	{
#if	ASTERISK_SRC_VERSION < 1100
		ast_log(LOG_ERROR, "create_member unable to parse member data: channel name = %s, data = %s\n", chan->name, data);
#else
		ast_log(LOG_ERROR, "create_member unable to parse member data: channel name = %s, data = %s\n", ast_channel_name(chan), data);
#endif
		ast_free(member);
		return NULL;
	}

	// parse the flags
	if ((token = strsep(&stringp, argument_delimiter)))
	{
		strncpy(member->flags, token, MEMBER_FLAGS_LEN);
	}

	while ((token = strsep(&stringp, argument_delimiter)))
	{
#if	SILDET == 2
		static const char arg_vad_prob_start[] = "vad_prob_start";
		static const char arg_vad_prob_continue[] = "vad_prob_continue";
#endif
		static const char arg_max_users[] = "max_users";
		static const char arg_conf_type[] = "type";
		static const char arg_chanspy[] = "spy";

		char *value = token;
		const char *key = strsep(&value, "=");
		
		if (!key || !value)
		{
			ast_log(LOG_WARNING, "Incorrect argument %s\n", token);
			continue;
		}

		if (!strncasecmp(key, arg_max_users, sizeof(arg_max_users) - 1))
		{
			member->max_users = strtol(value, (char **)NULL, 10);
		} else if (!strncasecmp(key, arg_conf_type, sizeof(arg_conf_type) - 1))
		{
			strncpy(member->type, value, MEMBER_TYPE_LEN);
		} else if (!strncasecmp(key, arg_chanspy, sizeof(arg_chanspy) - 1))
		{
			member->spyee_channel_name = ast_malloc(strlen(value) + 1);
			strcpy(member->spyee_channel_name, value);
#if	SILDET == 2
		} else if (!strncasecmp(key, arg_vad_prob_start, sizeof(arg_vad_prob_start) - 1))
		{
			member->vad_prob_start = strtof(value, (char **)NULL);
		} else if (!strncasecmp(key, arg_vad_prob_continue, sizeof(arg_vad_prob_continue) - 1))
		{
			member->vad_prob_continue = strtof(value, (char **)NULL);
#endif
		} else
		{
			ast_log(LOG_WARNING, "unknown parameter %s with value %s\n", key, value);
		}

	}

	//
	// initialize member with default values
	//

	// keep pointer to member's channel
	member->chan = chan;

	// check for enter/leave sounds
	{
		const char *tmp;
	    ast_channel_lock(chan);
	    member->join_sound = (tmp = "join") ?
	    ast_strdup(tmp) : NULL;
	    member->leave_sound = (tmp = "leave") ?
	    ast_strdup(tmp) : NULL;
	    ast_channel_unlock(chan);
	}
	//DEBUG("conference sounds: join '%s' leave '%s'\n", member->join_sound, member->leave_sound);


	// set default if no type parameter
	if (!(*(member->type))) {
		strcpy(member->type, AST_CONF_TYPE_DEFAULT);
	}

	// record start time
	member->time_entered = ast_tvnow();
	//
	// parse passed flags
	//

	// temp pointer to flags string
	char* flags = member->flags;

	int i;

	for (i = 0; i < strlen(flags); ++i)
	{
		{
			// flags are L, l, a, T, V, D, A, R, M, x
			switch (flags[i])
			{
				// mute/no_recv options
			case 'L':
				member->mute_audio = 1;
				break;
			case 'l':
				member->norecv_audio = 1;
				break;
#if	SILDET == 1
				//Telephone connection
			case 'a':
			case 'T':
				member->via_telephone = 1;
				break;
#elif	SILDET == 2
				//Telephone connection
			case 'a':
				member->vad_flag = 1;
			case 'T':
				member->via_telephone = 1;
				break;
			case 'V':
				member->vad_flag = 1;
				break;
				// speex preprocessing options
			case 'D':
				member->denoise_flag = 1;
				break;
			case 'A':
				member->agc_flag = 1;
				break;
#endif
				// dtmf/moderator options
			case 'R':
				member->dtmf_relay = 1;
				break;
			case 'M':
				member->ismoderator = 1;
				break;
			case 'x':
				member->kick_conferees = 1;
				break;
			default:
				break;
			}
		}
	}

	//
	// configure silence detection and preprocessing
	// if the user is coming in via the telephone,
	// and is not listen-only
	//
#if	SILDET == 1
	if (member->via_telephone)
	{
		// create a webrtc preprocessor

		if (!WebRtcVad_Create(&member->dsp))
		{
			WebRtcVad_Init(member->dsp);
#if	AST_CONF_VAD_MODE
			WebRtcVad_set_mode(member->dsp, AST_CONF_VAD_MODE);
#endif
		}
		else
		{
#if	ASTERISK_SRC_VERSION < 1100
			ast_log(LOG_WARNING, "unable to initialize member dsp, channel => %s\n", chan->name);
#else
			ast_log(LOG_WARNING, "unable to initialize member dsp, channel => %s\n", ast_channel_name(chan));
#endif
		}
	}
#elif	SILDET == 2
	if (member->via_telephone)
	{
		// create a speex preprocessor

		if ((member->dsp = speex_preprocess_state_init(AST_CONF_BLOCK_SAMPLES, AST_CONF_SAMPLE_RATE)))
		{
			// set speex preprocessor options
			speex_preprocess_ctl(member->dsp, SPEEX_PREPROCESS_SET_VAD, &(member->vad_flag));
			speex_preprocess_ctl(member->dsp, SPEEX_PREPROCESS_SET_DENOISE, &(member->denoise_flag));
			speex_preprocess_ctl(member->dsp, SPEEX_PREPROCESS_SET_AGC, &(member->agc_flag));

			speex_preprocess_ctl(member->dsp, SPEEX_PREPROCESS_SET_PROB_START, &member->vad_prob_start);
			speex_preprocess_ctl(member->dsp, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &member->vad_prob_continue);
		}
		else
		{
#if	ASTERISK_SRC_VERSION < 1100
			ast_log(LOG_WARNING, "unable to initialize member dsp, channel => %s\n", chan->name);
#else
			ast_log(LOG_WARNING, "unable to initialize member dsp, channel => %s\n", ast_channel_name(chan));
#endif
		}
	}
#endif

	// set translation paths
#if	SILDET == 1 || SILDET == 2
	if (member->dsp)
	{
#if	ASTERISK_SRC_VERSION < 1000
		member->to_dsp = ast_translator_build_path(AST_FORMAT_CONFERENCE, chan->readformat);
#else
#if	ASTERISK_SRC_VERSION >= 1100
		member->to_dsp = ast_translator_build_path(&ast_format_conference, ast_channel_readformat(chan));
#else
		member->to_dsp = ast_translator_build_path(&ast_format_conference, &chan->readformat);
#endif
#endif
	}
	else
	{
#if	ASTERISK_SRC_VERSION < 1000
		member->to_slinear = ast_translator_build_path(AST_FORMAT_CONFERENCE, member->chan->readformat);
#else
#if	ASTERISK_SRC_VERSION >= 1100
		member->to_slinear = ast_translator_build_path(&ast_format_conference, ast_channel_readformat(chan));
#else
		member->to_slinear = ast_translator_build_path(&ast_format_conference, &member->chan->readformat);
#endif
#endif
	}
#else
#if	ASTERISK_SRC_VERSION < 1000
	member->to_slinear = ast_translator_build_path(AST_FORMAT_CONFERENCE, member->chan->readformat);
#else
#if	ASTERISK_SRC_VERSION >= 1100
	member->to_slinear = ast_translator_build_path(&ast_format_conference, ast_channel_readformat(member->chan));
#else
	member->to_slinear = ast_translator_build_path(&ast_format_conference, &member->chan->readformat);
#endif
#endif
#endif
#if	ASTERISK_SRC_VERSION < 1000
	member->from_slinear = ast_translator_build_path(member->chan->writeformat, AST_FORMAT_CONFERENCE);
#else
#if	ASTERISK_SRC_VERSION >= 1100
	member->from_slinear = ast_translator_build_path(ast_channel_writeformat(member->chan), &ast_format_conference);
#else
	member->from_slinear = ast_translator_build_path(&member->chan->writeformat, &ast_format_conference);
#endif
#endif

	// index for converted_frames array
#if	ASTERISK_SRC_VERSION < 1000
	switch (member->chan->writeformat)
#else
#if	ASTERISK_SRC_VERSION < 1100
	switch (member->chan->writeformat.id)
#else
	switch (ast_channel_writeformat(member->chan)->id)
#endif
#endif
	{
		case AST_FORMAT_CONFERENCE:
			member->write_format_index = AC_CONF_INDEX;
			break;

		case AST_FORMAT_ULAW:
			member->write_format_index = AC_ULAW_INDEX;
			break;

	        case AST_FORMAT_ALAW:
			member->write_format_index = AC_ALAW_INDEX;
			break;

		case AST_FORMAT_GSM:
			member->write_format_index = AC_GSM_INDEX;
			break;
#ifdef	AC_USE_SPEEX
		case AST_FORMAT_SPEEX:
			member->write_format_index = AC_SPEEX_INDEX;
			break;
#endif
#ifdef AC_USE_G729A
		case AST_FORMAT_G729A:
			member->write_format_index = AC_G729A_INDEX;
			break;
#endif
#ifdef AC_USE_G722
		case AST_FORMAT_SLINEAR:
			member->write_format_index = AC_SLINEAR_INDEX;
			break;
		case AST_FORMAT_G722:
			member->write_format_index = AC_G722_INDEX;
			break;
#endif
		default:
			break;
	}

	// index for converted_frames array
#if	SILDET == 1 || SILDET == 2
#if	ASTERISK_SRC_VERSION < 1000
	switch (member->dsp ? AST_FORMAT_CONFERENCE : member->chan->readformat)
#else
#if	ASTERISK_SRC_VERSION < 1100
	switch (member->dsp ? AST_FORMAT_CONFERENCE : member->chan->readformat.id)
#else
	switch (member->dsp ? AST_FORMAT_CONFERENCE : ast_channel_readformat(member->chan)->id)
#endif
#endif
#else
#if	ASTERISK_SRC_VERSION < 1000
	switch (member->chan->readformat)
#else
#if	ASTERISK_SRC_VERSION < 1100
	switch (member->chan->readformat.id)
#else
	switch (ast_channel_readformat(member->chan)->id)
#endif
#endif
#endif
	{
		case AST_FORMAT_CONFERENCE:
			member->read_format_index = AC_CONF_INDEX;
			break;

		case AST_FORMAT_ULAW:
			member->read_format_index = AC_ULAW_INDEX;
			break;

		case AST_FORMAT_ALAW:
			member->read_format_index = AC_ALAW_INDEX;
			break;

		case AST_FORMAT_GSM:
			member->read_format_index = AC_GSM_INDEX;
			break;
#ifdef	AC_USE_SPEEX
		case AST_FORMAT_SPEEX:
			member->read_format_index = AC_SPEEX_INDEX;
			break;
#endif
#ifdef AC_USE_G729A
		case AST_FORMAT_G729A:
			member->read_format_index = AC_G729A_INDEX;
			break;
#endif
#ifdef AC_USE_G722
		case AST_FORMAT_SLINEAR:
			member->read_format_index = AC_SLINEAR_INDEX;
			break;
		case AST_FORMAT_G722:
			member->read_format_index = AC_G722_INDEX;
			break;
#endif
		default:
			break;
	}
	//
	// finish up
	//

	return member;
}

ast_conf_member* delete_member(ast_conf_member* member)
{
	ast_mutex_lock(&member->lock);

	member->delete_flag = 1;
	if (member->use_count)
		ast_cond_wait(&member->delete_var, &member->lock);

	ast_mutex_unlock(&member->lock);

	// destroy member mutex and condition variable
	ast_mutex_destroy(&member->lock);
	ast_cond_destroy(&member->delete_var);

	// destroy incoming/outgoing queue mutexes
	ast_mutex_destroy(&member->incomingq.lock);
	ast_mutex_destroy(&member->outgoingq.lock);

	//
	// delete the members frames
	//
	struct ast_frame* fr;

	// incoming frames
	while ((fr = AST_LIST_REMOVE_HEAD(&member->incomingq.frames, frame_list)))
		ast_frfree(fr);

	// outgoing frames
	while ((fr = AST_LIST_REMOVE_HEAD(&member->outgoingq.frames, frame_list)))
		ast_frfree(fr);

	// speaker buffer
	if (member->speakerBuffer)
	{
		ast_free(member->speakerBuffer);
	}
	// speaker frames
	if (member->mixAstFrame)
	{
		ast_free(member->mixAstFrame);
	}
	if (member->mixConfFrame)
	{
		ast_free(member->mixConfFrame);
	}

#if	SILDET == 1
	if (member->dsp)
	{
		WebRtcVad_Free(member->dsp);
		ast_translator_free_path(member->to_dsp);
	}
#elif	SILDET == 2
	if (member->dsp)
	{
		speex_preprocess_state_destroy(member->dsp);
		ast_translator_free_path(member->to_dsp);
	}
#endif

	// free the mixing translators
	ast_translator_free_path(member->to_slinear);
	ast_translator_free_path(member->from_slinear);

	// get a pointer to the next
	// member so we can return it
	ast_conf_member* nm = member->next;

	// free the member's copy of the spyee channel name
	if (member->spyee_channel_name)
	{
		ast_free(member->spyee_channel_name);
	}

	// free join/leave sound file names (NULL if not set)
	ast_free(member->join_sound);
	ast_free(member->leave_sound);

	// clear all sounds
	ast_conf_soundq *sound = member->soundq;
	ast_conf_soundq *next;

	while (sound)
	{
		next = sound->next;
		if (sound->stream)
			ast_stopstream(member->chan);
		ast_free(sound);
		sound = next;
	}

#ifdef	CACHE_CONTROL_BLOCKS
	// put the member control block on the free list
	ast_mutex_lock(&mbrblocklist_lock);
	member->next = mbrblocklist;
	mbrblocklist = member;
	ast_mutex_unlock(&mbrblocklist_lock);
#else
	ast_free(member);
#endif
	return nm;
}

//
// incoming frame functions
//

conf_frame* get_incoming_frame(ast_conf_member *member)
{
	if (!member->incomingq.count)
	{
		return NULL;
	}

	ast_mutex_lock(&member->incomingq.lock);

	// get first frame
	struct ast_frame* fr = AST_LIST_REMOVE_HEAD(&member->incomingq.frames, frame_list);

	// decrement frame count
	member->incomingq.count--;

	ast_mutex_unlock(&member->incomingq.lock);

	conf_frame *cfr  = create_conf_frame(member, NULL);

	if (cfr)
	{
		cfr->fr = fr;
	}
	else
	{
		ast_log(LOG_ERROR, "unable to malloc conf_frame\n");
		ast_frfree(fr);
	}

	return cfr;
}

void queue_incoming_frame(ast_conf_member* member, struct ast_frame* fr)
{
	//
	// create new frame from passed data frame
	//
	if (!(fr = ast_frdup(fr)))
	{
		ast_log(LOG_ERROR, "unable to malloc incoming ast_frame\n");
		return;
	}

	//
	// add new frame to members incoming frame queue
	// (i.e. save this frame data, so we can distribute it in conference_exec later)
	//

	ast_mutex_lock(&member->incomingq.lock);

	AST_LIST_INSERT_TAIL(&member->incomingq.frames, fr, frame_list);

	//
	// drop half of the queue if more than AST_CONF_MAX_QUEUE
	//
	if (++member->incomingq.count > AST_CONF_MAX_QUEUE)
	{
		int i = member->incomingq.count / 2;
		while(i--)
		{
			ast_frfree(AST_LIST_REMOVE_HEAD(&member->incomingq.frames, frame_list));
			member->incomingq.count--;
		}
	}

	ast_mutex_unlock(&member->incomingq.lock);
}

//
// outgoing frame functions
//

struct ast_frame* get_outgoing_frame(ast_conf_member *member)
{
	if (member->outgoingq.count)
	{
		ast_mutex_lock(&member->outgoingq.lock);

		struct ast_frame* fr = AST_LIST_REMOVE_HEAD(&member->outgoingq.frames, frame_list);

		// decrement frame count
		member->outgoingq.count--;
		ast_mutex_unlock(&member->outgoingq.lock);
		return fr;
	}

	return NULL;
}

void queue_outgoing_frame(ast_conf_member* member, struct ast_frame* fr, struct timeval delivery)
{
	//
	// create new frame from passed data frame
	//
	if (!(fr  = ast_frdup(fr)))
	{
		ast_log(LOG_ERROR, "unable to malloc outgoing ast_frame\n");
		return;
	}

	// set delivery timestamp
	fr->delivery = delivery;

	//
	// add new frame to members outgoing frame queue
	//

	ast_mutex_lock(&member->outgoingq.lock);

	AST_LIST_INSERT_TAIL(&member->outgoingq.frames, fr, frame_list);

	//
	// drop a frame if more than AST_CONF_MAX_QUEUE
	//
	if (++member->outgoingq.count > AST_CONF_MAX_QUEUE)
	{
		ast_frfree(AST_LIST_REMOVE_HEAD(&member->outgoingq.frames, frame_list));
		member->outgoingq.count--;
	}

	ast_mutex_unlock(&member->outgoingq.lock);
}

void queue_frame_for_listener(
	ast_conference* conf,
	ast_conf_member* member
)
{
	struct ast_frame* qf;
	conf_frame* frame = conf->listener_frame;

	if (frame)
	{
		// try for a pre-converted frame; otherwise, convert (and store) the frame
		if (!(qf = !member->listen_volume && !frame->talk_volume ? frame->converted[member->write_format_index] : 0))
		{
			if (!member->listen_volume)
			{
				qf = frame->fr;
			}
			else
			{
				// make a copy of the slinear version of the frame
				if (!(qf = ast_frdup(frame->fr)))
				{
					ast_log(LOG_WARNING, "unable to duplicate frame\n");
					queue_silent_frame(conf, member);
					return;
				}

				ast_frame_adjust_volume(qf, member->listen_volume);
			}

			// convert using the conference's translation path
			qf = convert_frame(conf->from_slinear_paths[member->write_format_index], qf, member->listen_volume);

			// store the converted frame
			// (the frame will be free'd next time through the loop)
			if (!member->listen_volume)
			{
				if (frame->converted[member->write_format_index] && conf->from_slinear_paths[member->write_format_index])
					ast_frfree(frame->converted[member->write_format_index]);
				frame->converted[member->write_format_index] = qf;
				frame->talk_volume = 0;
			}
		}

		if (qf)
		{
			queue_outgoing_frame(member, qf, conf->delivery_time);

			if (member->listen_volume)
			{
				// free frame (the translator's copy)
				if (conf->from_slinear_paths[member->write_format_index])
					ast_frfree(qf);
			}
		}
		else
		{
#if	ASTERISK_SRC_VERSION < 1100
			ast_log(LOG_WARNING, "unable to translate outgoing listener frame, channel => %s\n", member->chan->name);
#else
			ast_log(LOG_WARNING, "unable to translate outgoing listener frame, channel => %s\n", ast_channel_name(member->chan));
#endif
		}

	}
	else
	{
		queue_silent_frame(conf, member);
	}
}

void queue_frame_for_speaker(
	ast_conference* conf,
	ast_conf_member* member
)
{
	struct ast_frame* qf;
	conf_frame* frame = member->speaker_frame;

	if (frame)
	{
		//
		// convert and queue frame
		//

		if ((qf = frame->converted[member->write_format_index]) && !member->listen_volume && !frame->talk_volume)
		{
			// frame is already in correct format, so just queue it

			queue_outgoing_frame(member, qf, conf->delivery_time);
		}
		else
		{
			if (member->listen_volume)
			{
				ast_frame_adjust_volume(frame->fr, member->listen_volume);
			}

			//
			// convert frame to member's write format
			//
			qf = convert_frame(member->from_slinear, frame->fr, 0);

			if (qf)
			{
				// queue frame
				queue_outgoing_frame(member, qf, conf->delivery_time);

				// free frame (the translator's copy)
				if (member->from_slinear)
					ast_frfree(qf);
			}
			else
			{
#if	ASTERISK_SRC_VERSION < 1100
				ast_log(LOG_WARNING, "unable to translate outgoing speaker frame, channel => %s\n", member->chan->name);
#else
				ast_log(LOG_WARNING, "unable to translate outgoing speaker frame, channel => %s\n", ast_channel_name(member->chan));
#endif
			}
		}
	}
	else
	{
		queue_silent_frame(conf, member);
	}
}



void queue_silent_frame(
	ast_conference* conf,
	ast_conf_member* member
)
{
	// get the appropriate silent frame
	struct ast_frame* qf = silent_conf_frame->converted[member->write_format_index];

	if (!qf)
	{
#if	ASTERISK_SRC_VERSION < 1000
		struct ast_trans_pvt* trans = ast_translator_build_path(member->chan->writeformat, AST_FORMAT_CONFERENCE);
#else
#if	ASTERISK_SRC_VERSION < 1100
		struct ast_trans_pvt* trans = ast_translator_build_path(&member->chan->writeformat, &ast_format_conference);
#else
		struct ast_trans_pvt* trans = ast_translator_build_path(ast_channel_writeformat(member->chan), &ast_format_conference);
#endif
#endif

		if (trans)
		{
			// translate the frame
			if ((qf = ast_translate(trans, silent_conf_frame->fr, 0)))
			{
				// isolate the frame so we can keep it around after trans is free'd
				qf = ast_frisolate(qf);

				// cache the new, isolated frame
				silent_conf_frame->converted[member->write_format_index] = qf;
			}

			ast_translator_free_path(trans);
		}
	}

	// if it's not null queue the frame
	if (qf)
	{
		queue_outgoing_frame(member, qf, conf->delivery_time);
	}
	else
	{
#if	ASTERISK_SRC_VERSION < 1100
		ast_log(LOG_ERROR, "unable to translate outgoing silent frame, channel => %s\n", member->chan->name);
#else
		ast_log(LOG_ERROR, "unable to translate outgoing silent frame, channel => %s\n", ast_channel_name(member->chan));
#endif
	}
}



void member_process_outgoing_frames(ast_conference* conf,
				  ast_conf_member *member)
{
	// skip members that are not ready
	// skip no receive audio clients
	if (!member->ready_for_outgoing || member->norecv_audio)
	{
		return;
	}

	if (!member->spy_partner)
	{
		// neither a spyer nor a spyee
		if (!member->is_speaking) 
		{
			// queue listener frame
			queue_frame_for_listener(conf, member);
		}
		else
		{
			// queue speaker frame
			queue_frame_for_speaker(conf, member);
		}
	}
	else
	{
		// either a spyer or a spyee
		if (member->spyee_channel_name)
		{
			// spyer -- always use member translator
			queue_frame_for_speaker(conf, member);
		}
		else
		{
			// spyee -- use member translator if spyee speaking or spyer whispering to spyee
			if (member->is_speaking || member->spy_partner->is_speaking)
			{
				queue_frame_for_speaker(conf, member);
			}
			else
			{
				queue_frame_for_listener(conf, member);
			}
		}
	}
}

void member_process_spoken_frames(ast_conference* conf,
				 ast_conf_member *member,
				 conf_frame **spoken_frames,
				 int *listener_count,
				 int *speaker_count
	)
{
	conf_frame *cfr;

	// handle retrieved frames
	if (!(cfr  = get_incoming_frame(member)))
	{
		// clear speaking state
		member->is_speaking = 0;

		// increment listener count
		(*listener_count)++;
	}
	else
	{
		// set speaking state
		member->is_speaking = 1;

		// add the frame to the list of spoken frames
		if (*spoken_frames)
		{
			cfr->next = *spoken_frames;
			(*spoken_frames)->prev = cfr;
		}

		// point the list at the new frame
		*spoken_frames = cfr;

		// increment speaker count
		(*speaker_count)++;
	}

	return;
}

// Add sound file to a member's sound queue
void member_play_sound ( struct ast_conf_member *member, const char *filename )
{
	struct ast_conf_soundq *newsound;
	struct ast_conf_soundq **q;

	newsound = calloc(1,sizeof(struct ast_conf_soundq));

	ast_mutex_lock(&member->lock);
	if (newsound == NULL) {
		perror("calloc for struct ast_conf_soundq failed");
	}
	else
	{
#if	ASTERISK_SRC_VERSION < 1100
		newsound->stream = ast_openstream(member->chan, filename, member->chan->language);
#else
		newsound->stream = ast_openstream(member->chan, filename, ast_channel_language(member->chan));
#endif
		if (newsound->stream)
		{
//#if ASTERISK_SRC_VERSION < 1100
//			member->chan->stream = NULL;
//#else
//			ast_channel_stream_set(member->chan, NULL);
//#endif
			ast_copy_string(newsound->name, filename, sizeof(newsound->name));
			// append sound to the end of the list.
			for (q=&member->soundq; *q; q = &((*q)->next));
			*q = newsound;

#if ASTERISK_SRC_VERSION < 1100
			ast_log( LOG_VERBOSE, "Playing conference message %s to channel %s\n", filename, member->chan->name ) ;
#else
			ast_log( LOG_VERBOSE, "Playing conference message %s to channel %s\n", filename, ast_channel_name(member->chan)) ;
#endif
		}
		else
		{
			char fmt[256];
#if ASTERISK_SRC_VERSION < 1100
			ast_log(LOG_ERROR, "Unable to open %s (format %s): %s\n",
					filename, ast_getformatname_multiple(fmt, sizeof(fmt), member->chan->nativeformats), strerror(errno));
#else
			ast_log(LOG_ERROR, "Unable to open %s (format %s): %s\n",
								filename, ast_getformatname_multiple(fmt, sizeof(fmt), ast_channel_nativeformats(member->chan)), strerror(errno));
#endif
			ast_free(newsound);
		}
	}
	ast_mutex_unlock(&member->lock);
}
