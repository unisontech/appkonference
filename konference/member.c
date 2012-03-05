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
#include "member.h"
#include "frame.h"

#include "asterisk/musiconhold.h"

#ifdef	CACHE_CONTROL_BLOCKS
AST_MUTEX_DEFINE_STATIC(mbrblocklist_lock);

#ifdef	SPEAKER_SCOREBOARD
static int last_score_id = 0 ;
AST_MUTEX_DEFINE_STATIC(speaker_scoreboard_lock);
#endif

#endif

// process an incoming frame.  Returns 0 normally, 1 if hangup was received.
static int process_incoming(ast_conf_member *member, ast_conference *conf, struct ast_frame *f)
{
	if (f->frametype == AST_FRAME_VOICE)
	{
		if ( member->mute_audio
			|| member->muted
			||  conf->membercount == 1)
		{
			// free the input frame
			ast_frfree( f ) ;
			return 0;
		}
#if	SILDET == 1 || SILDET == 2
		// reset silence detection flag
		int is_silent_frame = 0 ;
		//
		// make sure we have a valid dsp and frame type
		//
		if (member->dsp
#ifndef	AC_USE_G722
#if	ASTERISK == 14 || ASTERISK == 16
			&& f->subclass == AST_FORMAT_SLINEAR
#else
			&& f->subclass.integer == AST_FORMAT_SLINEAR
#endif
#else
#if	ASTERISK == 14 || ASTERISK == 16
			&& f->subclass == AST_FORMAT_SLINEAR16
#else
			&& f->subclass.integer == AST_FORMAT_SLINEAR16
#endif
#endif
			&& f->datalen == AST_CONF_FRAME_DATA_SIZE
			)
		{
			// send the frame to the preprocessor
#if	SILDET == 1
#if	ASTERISK == 14
			if (!WebRtcVad_Process( member->dsp, AST_CONF_SAMPLE_RATE, f->data, AST_CONF_BLOCK_SAMPLES ))
#else
			if (!WebRtcVad_Process( member->dsp, AST_CONF_SAMPLE_RATE, f->data.ptr, AST_CONF_BLOCK_SAMPLES ))
#endif
#elif	SILDET == 2
#if	ASTERISK == 14
			if (!speex_preprocess( member->dsp, f->data, NULL ))
#else
			if (!speex_preprocess( member->dsp, f->data.ptr, NULL ))
#endif
#endif
			{
				//
				// we ignore the preprocessor's outcome if we've seen voice frames
				// in within the last AST_CONF_FRAMES_TO_SKIP frames
				//
				if ( member->ignore_vad_result > 0 )
				{
					// skip speex_preprocess(), and decrement counter
					if (!--member->ignore_vad_result ) {
#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
						*(speaker_scoreboard + member->score_id) = '\x00';
#else
						manager_event(
							EVENT_FLAG_CONF,
							"ConferenceState",
							"Channel: %s\r\n"
							"Flags: %s\r\n"
							"State: %s\r\n",
							member->chan->name,
							member->flags,
							"silent"
						) ;
#endif
					}
				}
				else
				{
					// set silent_frame flag
					is_silent_frame = 1 ;
				}
			}
			else
			{
				if (!member->ignore_vad_result) {
#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
					*(speaker_scoreboard + member->score_id) = '\x01';
#else
					manager_event(
						EVENT_FLAG_CONF,
						"ConferenceState",
						"Channel: %s\r\n"
						"Flags: %s\r\n"
						"State: %s\r\n",
						member->chan->name,
						member->flags,
						"speaking"
					) ;
#endif
				}
				// voice detected, reset skip count
				member->ignore_vad_result = AST_CONF_FRAMES_TO_IGNORE ;
			}
		}
		if ( !is_silent_frame )
#endif
			queue_incoming_frame( member, f );
	}
	// In Asterisk 1.4 AST_FRAME_DTMF is equivalent to AST_FRAME_DTMF_END
	else if (f->frametype == AST_FRAME_DTMF)
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
				member->chan->uniqueid,
				member->chan->name,
#if	ASTERISK == 14 || ASTERISK == 16
				member->chan->cid.cid_num ? member->chan->cid.cid_num : "unknown",
				member->chan->cid.cid_name ? member->chan->cid.cid_name : "unknown",
				f->subclass,
#else
				member->chan->caller.id.number.str ? member->chan->caller.id.number.str : "unknown",
				member->chan->caller.id.name.str ? member->chan->caller.id.name.str: "unknown",
				f->subclass.integer,
#endif
				conf->membercount,
				member->flags,
				member->mute_audio
				) ;

		}
	}
	else if (f->frametype == AST_FRAME_CONTROL)
	{
#if	ASTERISK == 14 || ASTERISK == 16
		if (f->subclass == AST_CONTROL_HANGUP)
#else
		if (f->subclass.integer == AST_CONTROL_HANGUP)
#endif
		{
			// hangup received

			// free the input frame
			ast_frfree( f ) ;

			// break out of the while ( 42 == 42 )
			return 1;
		}
	}

	// free the input frame
	ast_frfree( f ) ;

	return 0;
}

// get the next frame from the soundq;  must be called with member locked.
static struct ast_frame *get_next_soundframe(ast_conf_member *member, struct ast_frame
    *exampleframe) {
    struct ast_frame *f;

again:
	ast_mutex_unlock(&member->lock);
again2:
    f=(member->soundq->stream && !member->soundq->stopped ? ast_readframe(member->soundq->stream) : NULL);

    if(!f) { // we're done with this sound; remove it from the queue, and try again
	ast_conf_soundq *toboot = member->soundq;

	if (!toboot->stopped && !toboot->stream)
	{
		toboot->stream = ast_openstream(member->chan, toboot->name, member->chan->language);
		//ast_log( LOG_WARNING, "trying to play sound: name = %s, stream = %p\n", toboot->name, toboot->stream);
		if (toboot->stream)
		{
			member->chan->stream = NULL;
			goto again2;
		}
		//ast_log( LOG_WARNING, "trying to play sound, %s not found!?", toboot->name);
	}

	if (toboot->stream) {
		ast_closestream(toboot->stream);
		//ast_log( LOG_WARNING, "finished playing a sound: name = %s, stream = %p\n", toboot->name, toboot->stream);
		// notify applications via mgr interface that this sound has been played
		manager_event(
			EVENT_FLAG_CONF,
			"ConferenceSoundComplete",
			"Channel: %s\r\n"
			"Sound: %s\r\n",
			member->chan->name,
			toboot->name
		);
	}

	ast_mutex_lock( &member->lock ) ;
	member->soundq = toboot->next;

	ast_free(toboot);
	if(member->soundq) goto again;

	member->muted = 0;

	ast_mutex_unlock(&member->lock);

	// if we get here, we've gotten to the end of the queue; reset write format
	if ( ast_set_write_format( member->chan, member->write_format ) < 0 )
	{
		ast_log( LOG_ERROR, "unable to set write format to %d\n",
		    member->write_format ) ;
	}
    } else {
	// copy delivery from exampleframe
	f->delivery = exampleframe->delivery;
    }

    return f;
}


// process outgoing frames for the channel, playing either normal conference audio,
// or requested sounds
static int process_outgoing(ast_conf_member *member)
{
	struct ast_frame* cf ; // frame read from the output queue
	struct ast_frame *f;

	for(;;)
	{
		// acquire member mutex and grab a frame.
		cf = get_outgoing_frame( member ) ;

                // if there's no frames exit the loop.
		if ( !cf )
		{
			break;
		}


		struct ast_frame *realframe = f = cf;

		// if we're playing sounds, we can just replace the frame with the
		// next sound frame, and send it instead
		ast_mutex_lock( &member->lock ) ;
		if ( member->soundq )
		{
			f = get_next_soundframe(member, f);
			if ( !f )
			{
				// if we didn't get anything, just revert to "normal"
				f = realframe;
			}
		} else {
			if (member->moh_flag) {
				member->ready_for_outgoing = 0;
				do {
					ast_frfree( cf ) ;
				} while ( (cf = get_outgoing_frame( member ) ));
				ast_moh_start(member->chan, NULL, NULL);
				ast_mutex_unlock(&member->lock);
				return 0;
			}
			ast_mutex_unlock(&member->lock);
		}

		// send the voice frame
		ast_write( member->chan, f ) ;

		// clean up frame
		ast_frfree( cf ) ;
		
		// free sound frame
		if ( f != realframe )
			ast_frfree(f) ;

		if ( member->chan->_softhangup )
			return 1;

	}

	return 0;
}

//
// main member thread function
//

#if	ASTERISK == 14
int member_exec( struct ast_channel* chan, void* data )
#else
int member_exec( struct ast_channel* chan, const char* data )
#endif
{
	ast_conference *conf ;
	char conf_name[CONF_NAME_LEN + 1]  = { 0 };
	ast_conf_member *member ;

	struct ast_frame *f ; // frame received from ast_read()

	int left = 0 ;
	int res;

	//
	// If the call has not yet been answered, answer the call
	// Note: asterisk apps seem to check _state, but it seems like it's safe
	// to just call ast_answer.  It will just do nothing if it is up.
	// it will also return -1 if the channel is a zombie, or has hung up.
	//

	res = ast_answer( chan ) ;
	if ( res )
	{
		ast_log( LOG_ERROR, "unable to answer call\n" ) ;
		return -1 ;
	}

	//
	// create a new member for the conference
 	//

	member = create_member( chan, (const char*)( data ), conf_name ) ; // flags, name

	// unable to create member, return an error
	if ( member == NULL )
	{
		ast_log( LOG_ERROR, "unable to create member\n" ) ;
		return -1 ;
	}

	//
	// setup asterisk read/write formats
	//

	if ( ast_set_read_format( chan, member->read_format ) < 0 )
	{
		ast_log( LOG_ERROR, "unable to set read format to signed linear\n" ) ;
		delete_member( member ) ;
		return -1 ;
	}

	if ( ast_set_write_format( chan, member->write_format ) < 0 ) // AST_FORMAT_SLINEAR, chan->nativeformats
	{
		ast_log( LOG_ERROR, "unable to set write format to signed linear\n" ) ;
		delete_member( member ) ;
		return -1 ;
	}

	//
	// setup a conference for the new member
	//

	char max_users_flag = 0 ;
	conf = join_conference( member, conf_name, &max_users_flag ) ;

	if ( !conf )
	{
		ast_log( LOG_NOTICE, "unable to setup member conference %s: max_users_flag is %d\n", conf_name, max_users_flag ) ;
		delete_member( member) ;
		return (max_users_flag ? 0 : -1 ) ;
	}

	// add member to channel table
	member->bucket = &(channel_table[hash(member->chan->name) % CHANNEL_TABLE_SIZE]);

	AST_LIST_LOCK (member->bucket ) ;
	AST_LIST_INSERT_HEAD (member->bucket, member, hash_entry) ;
	AST_LIST_UNLOCK (member->bucket ) ;

	manager_event(
		EVENT_FLAG_CONF,
		"ConferenceJoin",
		"ConferenceName: %s\r\n"
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
		"Moderators: %d\r\n"
		"Count: %d\r\n",
		conf->name,
		member->type,
		member->chan->uniqueid,
		member->conf_id,
#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
		member->score_id,
#endif
		member->flags,
		member->chan->name,
#if	ASTERISK == 14 || ASTERISK == 16
		member->chan->cid.cid_num ? member->chan->cid.cid_num : "unknown",
		member->chan->cid.cid_name ? member->chan->cid.cid_name : "unknown",
#else
		member->chan->caller.id.number.str ? member->chan->caller.id.number.str : "unknown",
		member->chan->caller.id.name.str ? member->chan->caller.id.name.str: "unknown",
#endif
		conf->moderators,
		conf->membercount
	) ;

	// if spyer setup failed, set variable and exit conference
	if ( member->spyee_channel_name && !member->spyer )
	{
		remove_member( member, conf, conf_name ) ;
		pbx_builtin_setvar_helper(member->chan, "KONFERENCE", "SPYFAILED" );
		return 0 ;
	}

	//
	// process loop for new member ( this runs in it's own thread )
	//

	// tell conference_exec we're ready for frames
	member->ready_for_outgoing = 1 ;
	while ( 42 == 42 )
	{
		// make sure we have a channel to process
		if ( !chan )
		{
			ast_log( LOG_NOTICE, "member channel has closed\n" ) ;
			break ;
		}

		//-----------------//
		// INCOMING FRAMES //
		//-----------------//

		// wait for an event on this channel
		left = ast_waitfor( chan, AST_CONF_WAITFOR_LATENCY ) ;

		if ( left < 0 )
		{
			// an error occured
			ast_log(
				LOG_NOTICE,
				"an error occured waiting for a frame, channel => %s, error => %d\n",
				chan->name, left
			) ;
			break; // out of the 42==42
		}
		else if ( left == 0 )
		{
			// no frame has arrived yet
			// ast_log( LOG_NOTICE, "no frame available from channel, channel => %s\n", chan->name ) ;
		}
		else if ( left > 0 )
		{
			// a frame has come in before the latency timeout
			// was reached, so we process the frame

			f = ast_read( chan ) ;

			if ( !f )
			{
				// They probably want to hangup...
				break ;
			}

			// actually process the frame: break if we got hangup.
			if(process_incoming(member, conf, f)) break;

		}

		if ( member->moh_stop ) {
			ast_moh_stop(member->chan);
			member->moh_stop = 0;
		}

		//-----------------//
		// OUTGOING FRAMES //
		//-----------------//

		if ( !process_outgoing(member) )
			// back to process incoming frames
			continue ;
		else
			// they probably hungup...
			break ;
	}

	//
	// clean up
	//

	if ( member->chan->_softhangup == AST_SOFTHANGUP_ASYNCGOTO )
		pbx_builtin_setvar_helper(member->chan, "KONFERENCE", "KICKED" );

	remove_member( member, conf, conf_name ) ;
	return 0 ;
}

//
// manange member functions
//

ast_conf_member* create_member( struct ast_channel *chan, const char* data, char* conf_name )
{
	ast_conf_member *member;
#ifdef	CACHE_CONTROL_BLOCKS
#ifdef	SPEAKER_SCOREBOARD
	int score_id ;
#endif
	if ( mbrblocklist )
	{
		// get member control block from the free list
		ast_mutex_lock ( &mbrblocklist_lock ) ;
		member = mbrblocklist;
		mbrblocklist = mbrblocklist->next;
		ast_mutex_unlock ( &mbrblocklist_lock ) ;
#ifdef	SPEAKER_SCOREBOARD
		score_id = member->score_id ;
#endif
		memset(member,0,sizeof(ast_conf_member));
	}
	else
	{
#endif
		// allocate new member control block
		if ( !(member = ast_calloc( 1,  sizeof( ast_conf_member ) )) )
		{
			ast_log( LOG_ERROR, "unable to calloc ast_conf_member\n" ) ;
			return NULL ;
		}
#ifdef	CACHE_CONTROL_BLOCKS
#ifdef	SPEAKER_SCOREBOARD
		ast_mutex_lock ( &speaker_scoreboard_lock ) ;
		score_id = (last_score_id < SPEAKER_SCOREBOARD_SIZE ? ++last_score_id : 0) ;
		ast_mutex_unlock ( &speaker_scoreboard_lock ) ;
	}
	// initialize score board identifier
	member->score_id = score_id ;
	// initialize score board entry
	*(speaker_scoreboard + score_id) = '\x00';
#else
	}
#endif
#endif

	// initialize mutex
	ast_mutex_init( &member->lock ) ;

	// initialize cv
	ast_cond_init( &member->delete_var, NULL ) ;

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
	strncpy( argstr, data, sizeof(argstr) - 1 ) ;

	// point to the copied data
	char *stringp = argstr;

	// parse the id
	char *token;
	if ( ( token = strsep( &stringp, argument_delimiter ) ) )
	{
		strncpy( conf_name, token, CONF_NAME_LEN ) ;
	}
	else
	{
		ast_log( LOG_ERROR, "create_member unable to parse member data: channel name = %s, data = %s\n", chan->name, data ) ;
		ast_free( member ) ;
		return NULL ;
	}

	// parse the flags
	if ( ( token = strsep( &stringp, argument_delimiter ) ) )
	{
		strncpy( member->flags, token, MEMBER_FLAGS_LEN ) ;
	}

	while ( (token = strsep(&stringp, argument_delimiter )) )
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
		
		if ( !key || !value )
		{
			ast_log(LOG_WARNING, "Incorrect argument %s\n", token);
			continue;
		}

		if ( !strncasecmp(key, arg_max_users, sizeof(arg_max_users) - 1) )
		{
			member->max_users = strtol(value, (char **)NULL, 10);
		} else if ( !strncasecmp(key, arg_conf_type, sizeof(arg_conf_type) - 1) )
		{
			strncpy( member->type, value, MEMBER_TYPE_LEN ) ;
		} else if ( !strncasecmp(key, arg_chanspy, sizeof(arg_chanspy) - 1) )
		{
			member->spyee_channel_name = ast_malloc( strlen( value ) + 1 ) ;
			strcpy( member->spyee_channel_name, value ) ;
#if	SILDET == 2
		} else if ( !strncasecmp(key, arg_vad_prob_start, sizeof(arg_vad_prob_start) - 1) )
		{
			member->vad_prob_start = strtof(value, (char **)NULL);
		} else if ( !strncasecmp(key, arg_vad_prob_continue, sizeof(arg_vad_prob_continue) - 1) )
		{
			member->vad_prob_continue = strtof(value, (char **)NULL);
#endif
		} else
		{
			ast_log(LOG_WARNING, "unknown parameter %s with value %s\n", key, value) ;
		}

	}

	//
	// initialize member with default values
	//

	// keep pointer to member's channel
	member->chan = chan ;

	// set default if no type parameter
	if ( !(*(member->type)) ) {
		strcpy( member->type, AST_CONF_TYPE_DEFAULT ) ;
	}

	// record start time
	// init dropped frame timestamps
	// init state change timestamp
	member->time_entered =
		member->last_in_dropped =
		ast_tvnow();
	//
	// parse passed flags
	//

	// temp pointer to flags string
	char* flags = member->flags ;

	int i;

	for ( i = 0 ; i < strlen( flags ) ; ++i )
	{
		{
			// flags are L, l, a, T, V, D, A, R, M, x, H
			switch ( flags[i] )
			{
				// mute/no_recv options
			case 'L':
				member->mute_audio = 1;
				break ;
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
				member->vad_flag = 1 ;
			case 'T':
				member->via_telephone = 1;
				break;
			case 'V':
				member->vad_flag = 1 ;
				break ;
				// speex preprocessing options
			case 'D':
				member->denoise_flag = 1 ;
				break ;
			case 'A':
				member->agc_flag = 1 ;
				break ;
#endif
				// dtmf/moderator/video switching options
			case 'R':
				member->dtmf_relay = 1;
				break;
			case 'M':
				member->ismoderator = 1;
				break;
			case 'x':
				member->kick_conferees = 1;
				break;
			case 'H':
				member->hold_flag = 1;
				break;

			default:
				break ;
			}
		}
	}

	//
	// configure silence detection and preprocessing
	// if the user is coming in via the telephone,
	// and is not listen-only
	//
#if	SILDET == 1
	if ( member->via_telephone )
	{
		// create a webrtc preprocessor

		if ( !WebRtcVad_Create(&member->dsp) )
		{
			WebRtcVad_Init(member->dsp) ;
#if	AST_CONF_VAD_MODE
			WebRtcVad_set_mode(member->dsp, AST_CONF_VAD_MODE) ;
#endif
		}
		else
		{
			ast_log( LOG_WARNING, "unable to initialize member dsp, channel => %s\n", chan->name ) ;
		}
	}
#elif	SILDET == 2
	if ( member->via_telephone )
	{
		// create a speex preprocessor

		if ( (member->dsp = speex_preprocess_state_init( AST_CONF_BLOCK_SAMPLES, AST_CONF_SAMPLE_RATE ) ))
		{
			// set speex preprocessor options
			speex_preprocess_ctl( member->dsp, SPEEX_PREPROCESS_SET_VAD, &(member->vad_flag) ) ;
			speex_preprocess_ctl( member->dsp, SPEEX_PREPROCESS_SET_DENOISE, &(member->denoise_flag) ) ;
			speex_preprocess_ctl( member->dsp, SPEEX_PREPROCESS_SET_AGC, &(member->agc_flag) ) ;

			speex_preprocess_ctl( member->dsp, SPEEX_PREPROCESS_SET_PROB_START, &member->vad_prob_start ) ;
			speex_preprocess_ctl( member->dsp, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &member->vad_prob_continue ) ;
		}
		else
		{
			ast_log( LOG_WARNING, "unable to initialize member dsp, channel => %s\n", chan->name ) ;
		}
	}
#endif
	//
	// read, write, and translation options
	//

	// set member's audio formats, taking dsp preprocessing into account
	// ( chan->nativeformats, AST_FORMAT_SLINEAR, AST_FORMAT_ULAW, AST_FORMAT_GSM )
#if	SILDET == 1 || SILDET == 2
#ifndef	AC_USE_G722
	member->read_format = ( !member->dsp ? chan->nativeformats : AST_FORMAT_SLINEAR ) ;
#else
	member->read_format = ( !member->dsp ? chan->nativeformats : AST_FORMAT_SLINEAR16 ) ;
#endif
#else
	member->read_format = chan->nativeformats ;
#endif
	member->write_format = chan->nativeformats;
	// 1.2 or 1.3+
#ifdef AST_FORMAT_AUDIO_MASK

	member->read_format &= AST_FORMAT_AUDIO_MASK;
	member->write_format &= AST_FORMAT_AUDIO_MASK;
#endif

	//translation paths ( ast_translator_build_path() returns null if formats match )
#ifndef	AC_USE_G722
	member->to_slinear = ast_translator_build_path( AST_FORMAT_SLINEAR, member->read_format ) ;
	member->from_slinear = ast_translator_build_path( member->write_format, AST_FORMAT_SLINEAR ) ;
#else
	member->to_slinear = ast_translator_build_path( AST_FORMAT_SLINEAR16, member->read_format ) ;
	member->from_slinear = ast_translator_build_path( member->write_format, AST_FORMAT_SLINEAR16 ) ;
#endif

	// index for converted_frames array
	switch ( member->write_format )
	{
#ifndef	AC_USE_G722
		case AST_FORMAT_SLINEAR:
#else
		case AST_FORMAT_SLINEAR16:
#endif
			member->write_format_index = AC_SLINEAR_INDEX ;
			break ;

		case AST_FORMAT_ULAW:
			member->write_format_index = AC_ULAW_INDEX ;
			break ;

	        case AST_FORMAT_ALAW:
			member->write_format_index = AC_ALAW_INDEX ;
			break ;

		case AST_FORMAT_GSM:
			member->write_format_index = AC_GSM_INDEX ;
			break ;
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
		case AST_FORMAT_G722:
			member->write_format_index = AC_G722_INDEX;
			break;
#endif
		default:
			break;
	}

	// index for converted_frames array
	switch ( member->read_format )
	{
#ifndef	AC_USE_G722
		case AST_FORMAT_SLINEAR:
#else
		case AST_FORMAT_SLINEAR16:
#endif
			member->read_format_index = AC_SLINEAR_INDEX ;
			break ;

		case AST_FORMAT_ULAW:
			member->read_format_index = AC_ULAW_INDEX ;
			break ;

		case AST_FORMAT_ALAW:
			member->read_format_index = AC_ALAW_INDEX ;
			break ;

		case AST_FORMAT_GSM:
			member->read_format_index = AC_GSM_INDEX ;
			break ;
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

	return member ;
}

ast_conf_member* delete_member( ast_conf_member* member )
{
	ast_mutex_lock ( &member->lock ) ;

	member->delete_flag = 1 ;
	if ( member->use_count )
		ast_cond_wait ( &member->delete_var, &member->lock ) ;

	ast_mutex_unlock ( &member->lock ) ;

	// destroy member mutex and condition variable
	ast_mutex_destroy( &member->lock ) ;
	ast_cond_destroy( &member->delete_var ) ;

	//
	// delete the members frames
	//
	struct ast_frame* fr ;

	// incoming frames
	while ( (fr = AST_LIST_REMOVE_HEAD(&member->inFrames, frame_list)))
		ast_frfree(fr) ;

	// outgoing frames
	while ( (fr = AST_LIST_REMOVE_HEAD(&member->outFrames, frame_list)))
		ast_frfree(fr) ;

	// speaker buffer
	if ( member->speakerBuffer )
	{
		ast_free( member->speakerBuffer ) ;
	}
	// speaker frames
	if ( member->mixAstFrame )
	{
		ast_free( member->mixAstFrame ) ;
	}
	if ( member->mixConfFrame )
	{
		ast_free( member->mixConfFrame );
	}

#if	SILDET == 1
	if ( member->dsp )
	{
		WebRtcVad_Free( member->dsp ) ;
	}
#elif	SILDET == 2
	if ( member->dsp )
	{
		speex_preprocess_state_destroy( member->dsp ) ;
	}
#endif

	// free the mixing translators
	ast_translator_free_path( member->to_slinear ) ;
	ast_translator_free_path( member->from_slinear ) ;

	// get a pointer to the next
	// member so we can return it
	ast_conf_member* nm = member->next ;

	// free the member's copy of the spyee channel name
	if ( member->spyee_channel_name )
	{
		ast_free( member->spyee_channel_name );
	}

	// clear all sounds
	ast_conf_soundq *sound = member->soundq;
	ast_conf_soundq *next;

	while ( sound )
	{
		next = sound->next;
		if ( sound->stream )
			ast_closestream( sound->stream );
		ast_free( sound );
		sound = next;
	}

#ifdef	CACHE_CONTROL_BLOCKS
	// put the member control block on the free list
	ast_mutex_lock ( &mbrblocklist_lock ) ;
	member->next = mbrblocklist;
	mbrblocklist = member;
	ast_mutex_unlock ( &mbrblocklist_lock ) ;
#else
	ast_free( member ) ;
#endif
	return nm ;
}

//
// incoming frame functions
//

conf_frame* get_incoming_frame( ast_conf_member *member )
{
	ast_mutex_lock(&member->lock);

	if ( !member->inFramesCount )
	{
		ast_mutex_unlock(&member->lock);
		return NULL ;
	}

	// get first frame
	struct ast_frame* fr = AST_LIST_REMOVE_HEAD(&member->inFrames, frame_list);

	// decrement frame count
	member->inFramesCount-- ;

	ast_mutex_unlock(&member->lock);

	conf_frame *cfr  = create_conf_frame(member, NULL );

	if ( cfr )
	{
		cfr->fr = fr ;
	}
	else
	{
		ast_log( LOG_ERROR, "unable to malloc conf_frame\n" ) ;
		ast_frfree( fr ) ;
	}

	return cfr ;
}

void queue_incoming_frame( ast_conf_member* member, struct ast_frame* fr )
{
	ast_mutex_lock(&member->lock);

	if ( member->inFramesCount > member->inFramesNeeded )
	{
		if ( member->inFramesCount > AST_CONF_QUEUE_DROP_THRESHOLD )
		{
			struct timeval curr = ast_tvnow();

			// time since last dropped frame
			long diff = ast_tvdiff_ms(curr, member->last_in_dropped);

			// number of milliseconds which must pass between frame drops
			// ( 15 frames => -100ms, 10 frames => 400ms, 5 frames => 900ms, 0 frames => 1400ms, etc. )
			long time_limit = 1000 - ( ( member->inFramesCount - AST_CONF_QUEUE_DROP_THRESHOLD ) * 100 ) ;

			if ( diff >= time_limit )
			{
				// drop a frame
				ast_frfree( AST_LIST_REMOVE_HEAD(&member->inFrames, frame_list) );
				member->inFramesCount-- ;

				member->last_in_dropped = ast_tvnow();
			}
		}
	}

	//
	// if we have to drop frames, we'll drop new frames
	// because it's easier ( and doesn't matter much anyway ).
	//
	if ( member->inFramesCount >= AST_CONF_MAX_QUEUE )
	{
		ast_mutex_unlock(&member->lock);
		return ;
	}

	//
	// create new frame from passed data frame
	//
	if ( !(fr = ast_frdup( fr )) )
	{
		ast_log( LOG_ERROR, "unable to malloc incoming ast_frame\n" ) ;
		ast_mutex_unlock(&member->lock);
		return ;
	}

	//
	// add new frame to speaking members incoming frame queue
	// ( i.e. save this frame data, so we can distribute it in conference_exec later )
	//
	AST_LIST_INSERT_TAIL(&member->inFrames, fr, frame_list);

	member->inFramesCount++ ;

	ast_mutex_unlock(&member->lock);
}

//
// outgoing frame functions
//

struct ast_frame* get_outgoing_frame( ast_conf_member *member )
{
	ast_mutex_lock(&member->lock);

	if ( member->outFramesCount > AST_CONF_MIN_QUEUE )
	{
		struct ast_frame* fr = AST_LIST_REMOVE_HEAD(&member->outFrames, frame_list);

		// decrement frame count
		member->outFramesCount-- ;
		ast_mutex_unlock(&member->lock);
		return fr ;
	}

	ast_mutex_unlock(&member->lock);

	return NULL ;
}

void queue_outgoing_frame( ast_conf_member* member, struct ast_frame* fr, struct timeval delivery )
{
	//
	// we have to drop frames, so we'll drop new frames
	// because it's easier ( and doesn't matter much anyway ).
	//
	if ( member->outFramesCount >= AST_CONF_MAX_QUEUE )
	{
		return ;
	}

	//
	// create new frame from passed data frame
	//
	if ( !(fr  = ast_frdup(fr)) )
	{
		ast_log( LOG_ERROR, "unable to malloc outgoing ast_frame\n" ) ;
		return ;
	}

	// set delivery timestamp
	fr->delivery = delivery ;

	//
	// add new frame to speaking members outgoing frame queue
	//
	AST_LIST_INSERT_TAIL(&member->outFrames, fr, frame_list);

	member->outFramesCount++ ;
}

void queue_frame_for_listener(
	ast_conference* conf,
	ast_conf_member* member
)
{
	struct ast_frame* qf ;
	conf_frame* frame = conf->listener_frame ;

	if ( frame )
	{
		// first, try for a pre-converted frame
		qf = (!member->listen_volume && !frame->talk_volume ? frame->converted[member->write_format_index] : 0);

		// convert ( and store ) the frame
		if ( !qf )
		{
			if (!member->listen_volume )
			{
				qf = frame->fr ;
			}
			else
			{
				// make a copy of the slinear version of the frame
				if ( !(qf = ast_frdup( frame->fr ))  )
				{
					ast_log( LOG_WARNING, "unable to duplicate frame\n" ) ;
					queue_silent_frame( conf, member ) ;
					return ;
				}

				ast_frame_adjust_volume(qf, member->listen_volume);
			}

			// convert using the conference's translation path
			qf = convert_frame( conf->from_slinear_paths[ member->write_format_index ], qf, member->listen_volume ) ;

			// store the converted frame
			// ( the frame will be free'd next time through the loop )
			if (!member->listen_volume)
			{
				if ( frame->converted[ member->write_format_index ] && conf->from_slinear_paths[ member->write_format_index ] )
					ast_frfree (frame->converted[ member->write_format_index ]);
				frame->converted[ member->write_format_index ] = qf ;
				frame->talk_volume = 0;
			}
		}

		if ( qf )
		{
			queue_outgoing_frame( member, qf, conf->delivery_time ) ;

			if ( member->listen_volume )
			{
				// free frame ( the translator's copy )
				if (conf->from_slinear_paths[ member->write_format_index ])
					ast_frfree( qf ) ;
			}
		}
		else
		{
			ast_log( LOG_WARNING, "unable to translate outgoing listener frame, channel => %s\n", member->chan->name ) ;
		}

	}
	else
	{
		queue_silent_frame( conf, member ) ;
	}
}

void queue_frame_for_speaker(
	ast_conference* conf,
	ast_conf_member* member
)
{
	struct ast_frame* qf ;
	conf_frame* frame = member->speaker_frame ;

	if ( frame )
	{
		//
		// convert and queue frame
		//

		if ( (qf = frame->converted[member->write_format_index]) && !member->listen_volume && !frame->talk_volume )
		{
			// frame is already in correct format, so just queue it

			queue_outgoing_frame( member, qf, conf->delivery_time ) ;
		}
		else
		{
			if (member->listen_volume )
			{
				ast_frame_adjust_volume(frame->fr, member->listen_volume);
			}

			//
			// convert frame to member's write format
			//
			qf = convert_frame( member->from_slinear, frame->fr, 0 ) ;

			if ( qf )
			{
				// queue frame
				queue_outgoing_frame( member, qf, conf->delivery_time ) ;

				// free frame ( the translator's copy )
				if (member->from_slinear)
					ast_frfree( qf ) ;
			}
			else
			{
				ast_log( LOG_WARNING, "unable to translate outgoing speaker frame, channel => %s\n", member->chan->name ) ;
			}
		}
	}
	else
	{
		queue_silent_frame( conf, member ) ;
	}
}



void queue_silent_frame(
	ast_conference* conf,
	ast_conf_member* member
)
{
	// get the appropriate silent frame
	struct ast_frame* qf = silent_conf_frame->converted[ member->write_format_index ] ;

	if ( !qf  )
	{
		//
		// we need to do this to avoid echo on the speaker's line.
		// translators seem to be single-purpose, i.e. they
		// can't be used simultaneously for multiple audio streams
		//
#ifndef AC_USE_G722
		struct ast_trans_pvt* trans = ast_translator_build_path( member->write_format, AST_FORMAT_SLINEAR ) ;
#else
		struct ast_trans_pvt* trans = ast_translator_build_path( member->write_format, AST_FORMAT_SLINEAR16 ) ;
#endif
		if ( trans )
		{
			int c;
			// attempt ( five times ) to get a silent frame
			// to make sure we provice the translator with enough data
			for ( c = 0 ; c < 5 ; ++c )
			{
				// translate the frame
				qf = ast_translate( trans, silent_conf_frame->fr, 0 ) ;

				// break if we get a frame
				if ( qf ) break ;
			}

			if ( qf )
			{
				// isolate the frame so we can keep it around after trans is free'd
				qf = ast_frisolate( qf ) ;

				// cache the new, isolated frame
				silent_conf_frame->converted[ member->write_format_index ] = qf ;
			}

			ast_translator_free_path( trans ) ;
		}
	}

	//
	// queue the frame, if it's not null,
	// otherwise there was an error
	//
	if ( qf )
	{
		queue_outgoing_frame( member, qf, conf->delivery_time ) ;
	}
	else
	{
		ast_log( LOG_ERROR, "unable to translate outgoing silent frame, channel => %s\n", member->chan->name ) ;
	}
}



void member_process_outgoing_frames(ast_conference* conf,
				  ast_conf_member *member)
{
	ast_mutex_lock(&member->lock);

	// skip members that are not ready
	// skip no receive audio clients
	if ( !member->ready_for_outgoing || member->norecv_audio == 1 )
	{
		ast_mutex_unlock(&member->lock);
		return ;
	}

	if ( !member->spy_partner )
	{
		// neither a spyer nor a spyee
		if ( !member->local_speaking_state ) 
		{
			// queue listener frame
			queue_frame_for_listener( conf, member ) ;
		}
		else
		{
			// queue speaker frame
			queue_frame_for_speaker( conf, member ) ;
		}
	}
	else
	{
		// either a spyer or a spyee
		if ( member->spyer != 0 )
		{
			// spyer -- always use member translator
			queue_frame_for_speaker( conf, member ) ;
		}
		else
		{
			// spyee -- use member translator if spyee speaking or spyer whispering to spyee
			if ( member->local_speaking_state == 1 || member->spy_partner->local_speaking_state == 1 )
			{
				queue_frame_for_speaker( conf, member ) ;
			}
			else
			{
				queue_frame_for_listener( conf, member ) ;
			}
		}
	}

	ast_mutex_unlock(&member->lock);
}

void member_process_spoken_frames(ast_conference* conf,
				 ast_conf_member *member,
				 conf_frame **spoken_frames,
				 long time_diff,
				 int *listener_count,
				 int *speaker_count
	)
{
	conf_frame *cfr;

	// acquire member mutex
	ast_mutex_lock( &member->lock ) ;

	// reset speaker frame
	member->speaker_frame = NULL ;

	// tell member the number of frames we're going to need ( used to help dropping algorithm )
	member->inFramesNeeded = ( time_diff / AST_CONF_FRAME_INTERVAL ) - 1 ;

	// non-listener member should have frames,
	// unless silence detection dropped them
	cfr = get_incoming_frame( member ) ;

	// handle retrieved frames
	if ( !cfr || !cfr->fr  )
	{
		// Decrement speaker count for us and for driven members
		// This happens only for the first missed frame, since we want to
		// decrement only on state transitions
		if ( member->local_speaking_state == 1 )
		{
			member->local_speaking_state = 0;
		}
		// count the listeners
		(*listener_count)++ ;
	}
	else
	{
		// append the frame to the list of spoken frames
		if ( *spoken_frames )
		{
			// add new frame to end of list
			cfr->next = *spoken_frames ;
			(*spoken_frames)->prev = cfr ;
		}

		// point the list at the new frame
		*spoken_frames = cfr ;
		// Increment speaker count for us and for driven members
		// This happens only on the first received frame, since we want to
		// increment only on state transitions
		if ( !member->local_speaking_state )
		{
			member->local_speaking_state = 1;
		}
		// count the speakers
		(*speaker_count)++ ;
	}

	// release member mutex
	ast_mutex_unlock( &member->lock ) ;

	return;
}
