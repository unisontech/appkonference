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

#include "asterisk/paths.h"
#include "asterisk/autoconfig.h"
#include "conference.h"
#include "frame.h"
#include "asterisk/utils.h"
#include "asterisk/format.h"

#include "asterisk/app.h"
#include "asterisk/say.h"

#include "asterisk/musiconhold.h"

#ifdef	TIMERFD
#include <sys/timerfd.h>
#include <errno.h>
#elif	KQUEUE
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#endif

#if	ASTERISK_SRC_VERSION > 108
struct ast_format ast_format_conference = { .id = AST_FORMAT_CONFERENCE };
struct ast_format ast_format_ulaw = { .id = AST_FORMAT_ULAW };
struct ast_format ast_format_alaw = { .id = AST_FORMAT_ALAW };
struct ast_format ast_format_gsm = { .id = AST_FORMAT_GSM };
#ifdef  AC_USE_SPEEX
struct ast_format ast_format_speex = { .id = AST_FORMAT_SPEEX };
#endif
#ifdef  AC_USE_G729A
struct ast_format ast_format_g729a = { .id = AST_FORMAT_G729A };
#endif
#ifdef  AC_USE_G722
struct ast_format ast_format_slinear = { .id = AST_FORMAT_SLINEAR };
struct ast_format ast_format_g722 = { .id = AST_FORMAT_G722 };
#endif
#ifdef  AC_USE_OPUS
struct ast_format ast_format_opus = { .id = AST_FORMAT_OPUS };
#endif
#endif

//
// static variables
//

// list of current conferences
static ast_conference *conflist;

// unique counter for conferences
static int conference_uniqueint;

#ifdef	TIMERFD
// timer file descriptor
static int timerfd = -1;
#elif	KQUEUE
// kqueue file descriptor
static int kqueuefd = -1;
static struct kevent inqueue;
static struct kevent outqueue;
#endif

// mutex for synchronizing access to conflist
AST_MUTEX_DEFINE_STATIC(conflist_lock);

static int conference_count;

// Forward function declarations
//static ast_conference* find_conf(const char* name);
//static void get_unison_event_server_node_variable(struct ast_channel* channel, char **varval, char *workspace, int wssize);
static ast_conference* create_conf(char* name, ast_conf_member* member);
static ast_conference* remove_conf(ast_conference* conf);
static void add_member(ast_conf_member* member, ast_conference* conf);

//
// main conference function
//

static void conference_exec()
{
	// thread frequency variable
	int tf_count = 0;

#if	defined(TIMERFD) || defined(KQUEUE)
	int tf_expirations = 0;
	int tf_max_expirations = 0;
#endif

	// timer timestamps
	struct timeval base, tf_base;

	// conference epoch - 20 milliseconds
	struct timeval epoch = ast_tv(0, AST_CONF_FRAME_INTERVAL * 1000);

	// set base timestamps
	base = tf_base = ast_tvnow();

	// current conference
	ast_conference *conf = NULL;
	// last conference list
	ast_conference *lastconflist = NULL;

	//
	// conference thread loop
	//

	while (42)
	{
#ifdef	TIMERFD
		uint64_t expirations;

		// wait for start of epoch
		if (read(timerfd, &expirations, sizeof(expirations)) == -1)
		{
			ast_log(LOG_ERROR, "unable to read timer!? %s\n", strerror(errno));
		}

#ifdef	TIMERFD_EXPIRATIONS
		// check expirations
		if (expirations != 1)
		{
			ast_log(LOG_NOTICE, "timer read expirations = %ld!?\n", expirations);
		}
#endif
		// update expirations
		tf_expirations += expirations;
		if (expirations > tf_max_expirations) tf_max_expirations = expirations;
		// process expirations
		for (; expirations; expirations--)
		{
#elif	KQUEUE
		// wait for start of epoch
		if (kevent(kqueuefd, &inqueue, 1, &outqueue, 1, NULL) == -1)
		{
			ast_log(LOG_NOTICE, "unable to read timer!? %s\n", strerror(errno));
		}
#ifdef	KQUEUE_EXPIRATIONS
		// check expirations
		if (outqueue.data != 1) 
		{
			ast_log(LOG_NOTICE, "kqueue expirations = %ld!?\n", outqueue.data);
		}
#endif
		// update expirations
		tf_expirations += outqueue.data;
		if (outqueue.data > tf_max_expirations) tf_max_expirations = outqueue.data;
		// process expirations
		for (; outqueue.data; outqueue.data--)
		{
#else
		// update the current timestamp
		struct timeval curr = ast_tvnow();

		// calculate difference in timestamps
		long time_diff = ast_tvdiff_ms(curr, base);

		// calculate time we should sleep
		long time_sleep = AST_CONF_FRAME_INTERVAL - time_diff;

		if (time_sleep > 0)
		{
			// sleep for time_sleep (as microseconds)
			usleep(time_sleep * 1000);
		}
#endif

		//
		// check thread frequency
		//

		if (++tf_count >= AST_CONF_FRAMES_PER_SECOND)
		{
			// update current timestamp
			struct timeval tf_curr = ast_tvnow();

			// compute timestamp difference
			long tf_diff = ast_tvdiff_ms(tf_curr, tf_base);

			// compute sampling frequency
			float tf_frequency = (float)(tf_diff) / (float)(tf_count);

			if ((tf_frequency <= (float)(AST_CONF_FRAME_INTERVAL - 1))
				|| (tf_frequency >= (float)(AST_CONF_FRAME_INTERVAL + 1)))
			{
#if	defined(TIMERFD) || defined(KQUEUE)
				ast_log(LOG_WARNING, "processed frame frequency variation, tf_count => %d, tf_diff => %ld, tf_frequency => %2.4f, tf_expirations = %d tf_max_expirations = %d\n", tf_count, tf_diff, tf_frequency, tf_expirations, tf_max_expirations);
#else
				ast_log(LOG_WARNING, "processed frame frequency variation, tf_count => %d, tf_diff => %ld, tf_frequency => %2.4f\n", tf_count, tf_diff, tf_frequency);
#endif
			}

			// reset values
			tf_base = tf_curr;
			tf_count = 0;
#if	defined(TIMERFD) || defined(KQUEUE)
			tf_expirations = 0;
			tf_max_expirations = 0;
#endif
		}

		//
		// process the conference list
		//

		// increment the timer base (it will be used later to timestamp outgoing frames)
		base = ast_tvadd(base, epoch);

		// get the first entry
		if (!ast_mutex_trylock(&conflist_lock))
		{
			conf = lastconflist = conflist;
			ast_mutex_unlock(&conflist_lock);
		}
		else
		{
			conf  = lastconflist;
		}

		while (conf)
		{
			// acquire the conference lock
			ast_rwlock_rdlock(&conf->lock);

			//
			// check if the conference is empty and if so
			// remove it and continue to the next conference
			//

			if (!conf->membercount)
			{
				if (ast_mutex_trylock(&conflist_lock))
				{
					ast_rwlock_unlock(&conf->lock);

					// get the next conference
					conf = conf->next;

					continue;
				}

				conf = remove_conf(conf);

				if (conf == conflist)
				{
					// update last conference list
					lastconflist = conf;
				}

				if (!conference_count)
				{
					// release the conference list lock
					ast_mutex_unlock(&conflist_lock);
#ifdef	TIMERFD
					// close timer file
					close(timerfd);
#elif	KQUEUE
					// close kqueue file
					close(kqueuefd);
#endif
					// exit the conference thread
					pthread_exit(NULL);
				}

				// release the conference list lock
				ast_mutex_unlock(&conflist_lock);

				continue; // next conference
			}

			//
			// process conference frames
			//

			// conference member
			ast_conf_member *member;

			// update the current delivery time
			conf->delivery_time = base;

			// reset speaker and listener count
			int speaker_count = 0;
			int listener_count = 0;

			// reset pointer lists
			conf_frame *spoken_frames = NULL;

			// loop over member list and retrieve incoming frames
			for (member = conf->memberlist; member; member = member->next)
			{
				member_process_spoken_frames(conf,member,&spoken_frames,
							     &listener_count, &speaker_count);
			}

			// mix incoming frames and get batch of outgoing frames
			conf_frame *send_frames = spoken_frames ? mix_frames(conf, spoken_frames, speaker_count, listener_count) : NULL;

			// loop over member list and send outgoing frames
			for (member = conf->memberlist; member; member = member->next)
			{
				member_process_outgoing_frames(conf, member);
			}

			// delete send frames
			while (send_frames)
			{
				if (send_frames->member)
					send_frames->member->speaker_frame = NULL; // reset speaker frame
				else
					conf->listener_frame = NULL; // reset listener frame

				send_frames = delete_conf_frame(send_frames);
			}

			// release conference lock
			ast_rwlock_unlock(&conf->lock);

			// get the next conference
			conf = conf->next;
		}
#if	defined(TIMERFD) || defined(KQUEUE)
		}
#endif
	}
}

//
// manange conference functions
//

// called by app_conference.c:load_module()
int init_conference(void)
{
	int i;
	//init channel entries
	for (i = 0; i < CHANNEL_TABLE_SIZE; i++)
		AST_LIST_HEAD_INIT (&channel_table[i]);

	//init conference entries
	for (i = 0; i < CONFERENCE_TABLE_SIZE; i++)
		AST_LIST_HEAD_INIT (&conference_table[i]);

	//set delimiter
	argument_delimiter = !strcmp(PACKAGE_VERSION,"1.4") ? "|" : ",";

#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
	//init speaker scoreboard
	int fd;
	if ((fd = open(SPEAKER_SCOREBOARD_FILE,O_CREAT|O_TRUNC|O_RDWR,0644)) > -1)
	{
		if ((ftruncate(fd, SPEAKER_SCOREBOARD_SIZE)) == -1)
		{
			ast_log(LOG_ERROR, "unable to truncate scoreboard file!?\n");
			close(fd);
			return -1;
		}

		if ((speaker_scoreboard = (char*)mmap(NULL, SPEAKER_SCOREBOARD_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
		{
			ast_log(LOG_ERROR,"unable to mmap speaker scoreboard!?\n");
			close(fd);
			return -1;
		}

		close(fd);
	}
	else
	{
		ast_log(LOG_ERROR, "unable to open scoreboard file!?\n");
		return -1;
	}
#endif
	return 0;
}


// called by app_conference.c:unload_module()
void dealloc_conference(void)
{
	int i;
	//destroy channel entires
	for (i = 0; i < CHANNEL_TABLE_SIZE; i++)
		AST_LIST_HEAD_DESTROY(&channel_table[i]);

	//destroy conference entries
	for (i = 0; i < CONFERENCE_TABLE_SIZE; i++)
		AST_LIST_HEAD_DESTROY(&conference_table[i]);

#ifdef	CACHE_CONTROL_BLOCKS
	//free conference blocks
	ast_conference *confblock;
	while (confblocklist)
	{
		confblock = confblocklist;
		confblocklist = confblocklist->next;
		ast_free(confblock);
	}
	
	//free member blocks
	ast_conf_member *mbrblock;
	while (mbrblocklist)
	{
		mbrblock = mbrblocklist;
		mbrblocklist = mbrblocklist->next;
		ast_free(mbrblock);
	}
#endif
	//free silent frames
	for (i = 1; i < AC_SUPPORTED_FORMATS; ++i)
		if (silent_conf_frame->converted[i]) ast_frfree(silent_conf_frame->converted[i]);

#ifdef	CACHE_CONF_FRAMES
	//free conf frames
	conf_frame *cfr;
	while ((cfr = AST_LIST_REMOVE_HEAD(&confFrameList, frame_list)))
	{
		ast_free(cfr);
	}
#endif

#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
	if (speaker_scoreboard)
		munmap(speaker_scoreboard, SPEAKER_SCOREBOARD_SIZE);
#endif
}

ast_conference* join_conference(ast_conf_member* member, char* conf_name, char* max_users_flag)
{
	ast_conference* conf = NULL;

	// acquire the conference list lock
	ast_mutex_lock(&conflist_lock);



	// look for an existing conference
	conf = find_conf(conf_name);

	// unable to find an existing conference, try to create one
	if (!conf)
	{
		// create the new conference with one member
		conf = create_conf(conf_name, member);

		// return an error if create_conf() failed
		// otherwise set the member's pointer to its conference
		if (!conf)
			ast_log(LOG_ERROR, "unable to find or create requested conference\n");
	}
	else
	{
		//
		// existing conference found, add new member to the conference
		//
		// once we call add_member(), this thread
		// is responsible for calling delete_member()
		//
		if (!member->max_users || (member->max_users > conf->membercount)) {
			add_member(member, conf);
		} else {
			pbx_builtin_setvar_helper(member->chan, "KONFERENCE", "MAXUSERS");
			*max_users_flag = 1;
			conf = NULL;
		}
	}

	// release the conference list lock
	ast_mutex_unlock(&conflist_lock);

	return conf;
}

// This function should be called with conflist_lock mutex being held
ast_conference* find_conf(const char* name)
{
	ast_conference *conf;
	struct conference_bucket *bucket = &(conference_table[hash(name) % CONFERENCE_TABLE_SIZE]);

	AST_LIST_LOCK(bucket);

	AST_LIST_TRAVERSE(bucket, conf, hash_entry)
		if (!strcmp(conf->name, name)) {
			break;
		}

	AST_LIST_UNLOCK(bucket);

	return conf;
}

 void get_unison_event_server_node_variable(struct ast_channel *channel, char **varval, char *workspace, int wssize)
{
	char varname[] = "unison_event_server_node";
	pbx_retrieve_variable(channel, varname, varval, workspace, wssize, NULL);
}

// This function should be called with conflist_lock held
static ast_conference* create_conf(char* name, ast_conf_member* member)
{
	//
	// allocate memory for conference
	//

	ast_conference *conf;

#ifdef	CACHE_CONTROL_BLOCKS
	if (confblocklist)
	{
		// get conference control block from the free list
		conf = confblocklist;
		confblocklist = confblocklist->next;
		memset(conf,0,sizeof(ast_conference));
	}
	else
	{
#endif
		// allocate new conference control block
		if (!(conf = ast_calloc(1, sizeof(ast_conference))))
		{
			ast_log(LOG_ERROR, "unable to malloc ast_conference\n");
			return NULL;
		}
#ifdef	CACHE_CONTROL_BLOCKS
	}
#endif

	//
	// initialize conference
	//

	// record start time
	conf->time_entered = ast_tvnow();

	// copy name to conference
	strncpy((char*)&(conf->name), name, sizeof(conf->name) - 1);

	// initialize the conference lock
	ast_rwlock_init(&conf->lock);

	// create unique confuid
	if (ast_strlen_zero(ast_config_AST_SYSTEM_NAME)) {
		snprintf(conf->conf_uid, MAX_CONF_UID, "%li.%d", (long) time(NULL),
				ast_atomic_fetchadd_int(&conference_uniqueint, 1));
	} else {
		snprintf(conf->conf_uid, MAX_CONF_UID, "%s-%li.%d", ast_config_AST_SYSTEM_NAME,
				(long) time(NULL), ast_atomic_fetchadd_int(&conference_uniqueint, 1));
	}

#if 1

	size_t format_list_size, i;
	const struct ast_format_list *format_list = ast_format_list_get(&format_list_size);
	for (i = 0; i < format_list_size; ++i) {
		switch(format_list[i].format.id) {
		case AST_FORMAT_ULAW:
			conf->from_slinear_paths[AC_ULAW_INDEX] = ast_translator_build_path(&format_list[i].format, &ast_format_conference);
			break;
		case AST_FORMAT_ALAW:
			conf->from_slinear_paths[AC_ALAW_INDEX] = ast_translator_build_path(&format_list[i].format, &ast_format_conference);
			break;
		case AST_FORMAT_GSM:
			conf->from_slinear_paths[AC_GSM_INDEX] = ast_translator_build_path(&format_list[i].format, &ast_format_conference);
			break;
#ifdef	AC_USE_SPEEX
		case AST_FORMAT_SPEEX:
			conf->from_slinear_paths[AC_SPEEX_INDEX] = ast_translator_build_path(&format_list[i].format, &ast_format_conference);
			break;
#endif
#ifdef AC_USE_G729A
		case AST_FORMAT_G729A:
			conf->from_slinear_paths[AC_G729A_INDEX] = ast_translator_build_path(&format_list[i].format, &ast_format_conference);
			break;
#endif
#ifdef AC_USE_G722
		case AST_FORMAT_SLINEAR:
			conf->from_slinear_paths[AC_SLINEAR_INDEX] = ast_translator_build_path(&format_list[i].format, &ast_format_conference);
			break;
		case AST_FORMAT_G722:
			conf->from_slinear_paths[AC_G722_INDEX] = ast_translator_build_path(&format_list[i].format, &ast_format_conference);
			break;
#endif
#ifdef AC_USE_OPUS
		case AST_FORMAT_OPUS:
			conf->from_slinear_paths[AC_OPUS_INDEX] = ast_translator_build_path(&format_list[i].format, &ast_format_conference);
			break;
#endif
		default:
			break;
		}
	}

	ast_format_list_destroy(format_list);

#else

	// build translation paths
	conf->from_slinear_paths[AC_CONF_INDEX] = NULL;
#if	ASTERISK_SRC_VERSION < 1000
	conf->from_slinear_paths[AC_ULAW_INDEX] = ast_translator_build_path(AST_FORMAT_ULAW, AST_FORMAT_CONFERENCE);
	conf->from_slinear_paths[AC_ALAW_INDEX] = ast_translator_build_path(AST_FORMAT_ALAW, AST_FORMAT_CONFERENCE);
	conf->from_slinear_paths[AC_GSM_INDEX] = ast_translator_build_path(AST_FORMAT_GSM, AST_FORMAT_CONFERENCE);
#else
	conf->from_slinear_paths[AC_ULAW_INDEX] = ast_translator_build_path(&ast_format_ulaw, &ast_format_conference);
	conf->from_slinear_paths[AC_ALAW_INDEX] = ast_translator_build_path(&ast_format_alaw, &ast_format_conference);
	conf->from_slinear_paths[AC_GSM_INDEX] = ast_translator_build_path(&ast_format_gsm, &ast_format_conference);
#endif
#ifdef	AC_USE_SPEEX
#if	ASTERISK_SRC_VERSION < 1000
	conf->from_slinear_paths[AC_SPEEX_INDEX] = ast_translator_build_path(AST_FORMAT_SPEEX, AST_FORMAT_CONFERENCE);
#else
	conf->from_slinear_paths[AC_SPEEX_INDEX] = ast_translator_build_path(&ast_format_speex, &ast_format_conference);
#endif
#endif
#ifdef AC_USE_G729A
#if	ASTERISK_SRC_VERSION < 1000
	conf->from_slinear_paths[AC_G729A_INDEX] = ast_translator_build_path(AST_FORMAT_G729A, AST_FORMAT_CONFERENCE);
#else
	conf->from_slinear_paths[AC_G729A_INDEX] = ast_translator_build_path(&ast_format_g729a, &ast_format_conference);
#endif
#endif
#ifdef AC_USE_G722
#if	ASTERISK_SRC_VERSION < 1000
	conf->from_slinear_paths[AC_SLINEAR_INDEX] = ast_translator_build_path(AST_FORMAT_SLINEAR, AST_FORMAT_CONFERENCE);
	conf->from_slinear_paths[AC_G722_INDEX] = ast_translator_build_path(AST_FORMAT_G722, AST_FORMAT_CONFERENCE);
#else
	conf->from_slinear_paths[AC_SLINEAR_INDEX] = ast_translator_build_path(&ast_format_slinear, &ast_format_conference);
	conf->from_slinear_paths[AC_G722_INDEX] = ast_translator_build_path(&ast_format_g722, &ast_format_conference);
#endif
#endif
#ifdef AC_USE_OPUS
	conf->from_slinear_paths[AC_OPUS_INDEX] = ast_translator_build_path(&ast_format_opus, &ast_format_conference);
#endif

#endif


	//
	// spawn thread for new conference, using conference_exec(conf)
	//
	if (!conflist)
	{

#ifdef	TIMERFD
		// create timer
		if ((timerfd = timerfd_create(CLOCK_MONOTONIC,TFD_CLOEXEC)) == -1)
		{
			ast_log(LOG_ERROR, "unable to create timer!? %s\n", strerror(errno));

			// clean up conference
			ast_free(conf);
			return NULL;
		}

		// set interval to epoch
		struct itimerspec timerspec = { .it_interval.tv_sec = 0,
						.it_interval.tv_nsec = AST_CONF_FRAME_INTERVAL * 1000000,
						.it_value.tv_sec = 0,
						.it_value.tv_nsec = 1 };

		// set timer
		if (timerfd_settime(timerfd, 0, &timerspec, 0) == -1)
		{
			ast_log(LOG_NOTICE, "unable to set timer!? %s\n", strerror(errno));

			close(timerfd);

			// clean up conference
			ast_free(conf);
			return NULL;
		}
#elif	KQUEUE
		// create timer
		if ((kqueuefd = kqueue()) == -1)
		{
			ast_log(LOG_ERROR, "unable to create timer!? %s\n", strerror(errno));

			// clean up conference
			ast_free(conf);
			return NULL;
		}

		// set interval to epoch
		EV_SET(&inqueue, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, AST_CONF_FRAME_INTERVAL, 0);
#endif

		pthread_t conference_thread; // conference thread id
		if (!(ast_pthread_create(&conference_thread, NULL, (void*)conference_exec, NULL)))
		{
			// detach the thread so it doesn't leak
			pthread_detach(conference_thread);

			// if realtime set fifo scheduling and bump priority
			if (ast_opt_high_priority)
			{
				int policy;
				struct sched_param param;

				pthread_getschedparam(conference_thread, &policy, &param);
				
				++param.sched_priority;
				policy = SCHED_FIFO;
				pthread_setschedparam(conference_thread, policy, &param);
			}
		}
		else
		{
			ast_log(LOG_ERROR, "unable to start conference thread for conference %s\n", conf->name);

			// clean up conference
			ast_free(conf);
			return NULL;
		}
	}

	// add the initial member
	add_member(member, conf);

	// prepend new conference to conflist
	if (conflist)
		conflist->prev = conf;
	conf->next = conflist;
	conflist = conf;

	// add member to channel table
	conf->bucket = &(conference_table[hash(conf->name) % CONFERENCE_TABLE_SIZE]);

	AST_LIST_LOCK(conf->bucket);
	AST_LIST_INSERT_HEAD(conf->bucket, conf, hash_entry);
	AST_LIST_UNLOCK(conf->bucket);

	// count new conference
	++conference_count;

	return conf;
}

//This function should be called with conflist_lock and conf->lock held
ast_conference *remove_conf(ast_conference *conf)
{

	ast_conference *conf_temp;

	//
	// do some frame clean up
	//

	int c;
	for (c = 0; c < AC_SUPPORTED_FORMATS; ++c)
	{
		// free the translation paths
		if (conf->from_slinear_paths[c])
		{
			ast_translator_free_path(conf->from_slinear_paths[c]);
		}
	}

	// speaker frames
	if (conf->mixAstFrame)
	{
		ast_free(conf->mixAstFrame);
	}
	if (conf->mixConfFrame)
	{
		ast_free(conf->mixConfFrame);
	}

	AST_LIST_LOCK(conf->bucket);
	AST_LIST_REMOVE(conf->bucket, conf, hash_entry);
	AST_LIST_UNLOCK(conf->bucket);

	// unlock and destroy read/write lock
	ast_rwlock_unlock(&conf->lock);
	ast_rwlock_destroy(&conf->lock);

	conf_temp = conf->next;

	if (conf->prev)
		conf->prev->next = conf->next;

	if (conf->next)
		conf->next->prev = conf->prev;

	if (conf == conflist)
		conflist = conf_temp;
#ifdef	CACHE_CONTROL_BLOCKS
	// put the conference control block on the free list
	conf->next = confblocklist;
	confblocklist = conf;
#else
	ast_free(conf);	
#endif
	// update conference count
	--conference_count;

	return conf_temp;

}

void end_conference(const char *name)
{
	ast_conference *conf;

	// acquire the conference list lock
	ast_mutex_lock(&conflist_lock);

	if ((conf = find_conf(name)))
	{
		// acquire the conference lock
		ast_rwlock_rdlock(&conf->lock);

		// get list of conference members
		ast_conf_member* member = conf->memberlist;

		// loop over member list and request hangup
		while (member)
		{
			member->kick_flag = 1;
			ast_queue_frame(member->chan, &ast_null_frame);

			// go on to the next member
			// (we have the conf lock, so we know this is okay)
			member = member->next;
		}

		// release the conference lock
		ast_rwlock_unlock(&conf->lock);
	}

	// release the conference list lock
	ast_mutex_unlock(&conflist_lock);
}

//
// member-related functions
//

// This function should be called with conflist_lock held
static void add_member(ast_conf_member *member, ast_conference *conf)
{
	ast_conf_member *other_member;
	// acquire the conference lock
	ast_rwlock_wrlock(&conf->lock);

	//
	// if spying, setup spyer/spyee
	//
	if (member->spyee_channel_name)
	{
		ast_conf_member *spyee = find_member(member->spyee_channel_name);

		if (spyee)
		{
			if (!spyee->spy_partner && spyee->conf == conf)
			{
				spyee->spy_partner = member;
				member->spy_partner = spyee;
			}

			if (!--spyee->use_count && spyee->delete_flag)
				ast_cond_signal(&spyee->delete_var);
			ast_mutex_unlock(&spyee->lock);
		}
	}

	// update conference count
	conf->membercount++;

	// update moderator count
	if (member->ismoderator)
		conf->moderators++;

	// calculate member identifier
	member->conf_id = !conf->memberlast ? 1 : conf->memberlast->conf_id + 1;

	//
	// add member to list
	//
	if (!conf->memberlist)
		conf->memberlist = conf->memberlast = member;
	else {
		member->prev = conf->memberlast; // dbl links
		conf->memberlast->next = member;
		conf->memberlast = member;
	}

	// set pointer to conference
	member->conf = conf;


	// play music-on-hold if there is only one member in conference
	// and stop if number of members getting at least two
	if (conf->membercount == 1)
	{
		start_moh_member(1, member);
	}
	else if (conf->membercount == 2)
	{
		stop_moh_member(1, conf->memberlist);
		// erase rest of MOH to let person hear join sound
//#if ASTERISK_SRC_VERSION < 1100
//		conf->memberlist->chan->stream = NULL;
//#else
//		ast_channel_stream_set(conf->memberlist->chan, NULL);
//#endif
	}

	// play join sound to all the rest members of this conference (except last one)
	if (conf->membercount >1)
	{
		other_member = conf->memberlist;
		while (other_member) {
			member_play_sound(other_member, "join");
			other_member = other_member->next;
		}
	}

	// release the conference lock
	ast_rwlock_unlock(&conf->lock);

	return;
}

void remove_member(ast_conf_member* member, ast_conference* conf, char* conf_name)
{
	int membercount;
	int moderators;
	ast_conf_member* member_stayed;

	ast_rwlock_wrlock(&conf->lock);

	//
	// remove member from list
	//
	if (!member->prev)
		conf->memberlist = member->next;
	else
		member->prev->next = member->next;

	if (member->next)
		member->next->prev =  member->prev; // dbl links

	if (conf->memberlast == member)
		conf->memberlast = member->prev;

	// update member count
	membercount = --conf->membercount;

	// play leave sound to all the rest members of this conference
	if (conf->membercount > 1)
	{
		member_stayed = conf->memberlist;
		while (member_stayed) {
			member_play_sound(member_stayed, "leave");
			member_stayed = member_stayed->next;
		}
	}

	// play music-on-hold if there is only one conference member left
	// and stop doing this if the last member left the conference
	if (conf->membercount == 1)
	{
		start_moh_member(1, conf->memberlist);
	} else if (conf->membercount == 0)
	{
		stop_moh_member(1, member);
	}

	// update moderator count
	moderators = !member->ismoderator ? conf->moderators : --conf->moderators;

	// if this the last moderator and the flag is set then kick the rest
	if (member->ismoderator && member->kick_conferees && !conf->moderators)
	{
		ast_conf_member *member_temp = conf->memberlist;
		while (member_temp)
		{
			member_temp->kick_flag = 1;
			ast_queue_frame(member_temp->chan, &ast_null_frame);
			member_temp = member_temp->next;
		}
	}

	//
	// if spying sever connection to spyee
	//
	if (member->spy_partner)
	{
		if (member->spyee_channel_name)
		{
			member->spy_partner->spy_partner = NULL;
		}
		else
		{
			member->spy_partner->kick_flag = 1;
			ast_queue_frame(member->spy_partner->chan, &ast_null_frame);
		}
	}

	ast_rwlock_unlock(&conf->lock);

	// remove member from channel table
	if (member->bucket)
	{
		AST_LIST_LOCK(member->bucket);
		AST_LIST_REMOVE(member->bucket, member, hash_entry);
		AST_LIST_UNLOCK(member->bucket);
	}

	char workspace[1024];
	char *varval = "<unknown>";
	get_unison_event_server_node_variable(member->chan, &varval, workspace, sizeof(workspace));

	// output to manager...
	manager_event(
		EVENT_FLAG_CONF,
		"ConferenceLeave",
		"ConferenceName: %s\r\n"
		"Conference UID: %s\r\n"
		"Type:  %s\r\n"
		"UniqueID: %s\r\n"
		"Member: %d\r\n"
		"Flags: %s\r\n"
		"Channel: %s\r\n"
		"CallerID: %s\r\n"
		"CallerIDName: %s\r\n"
		"UnisonEventServerNode: %s\r\n"
		"Duration: %ld\r\n"
		"Moderators: %d\r\n"
		"Count: %d\r\n",
		conf_name,
		conf->conf_uid,
		member->type,
#if	ASTERISK_SRC_VERSION < 1100
		member->chan->uniqueid,
#else
		ast_channel_uniqueid(member->chan),
#endif
		member->conf_id,
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
		(long)ast_tvdiff_ms(ast_tvnow(),member->time_entered) / 1000,
		moderators,
		membercount
	);

	// delete the member
	delete_member(member);
}

void list_conferences(int fd)
{
	int duration;
	char duration_str[10];

        // any conferences?
	if (conflist)
	{

		// acquire mutex
		ast_mutex_lock(&conflist_lock);

		ast_conference *conf = conflist;

		ast_cli(fd, "%-20.20s %-20.20s %-20.20s %-20.20s\n", "Name", "Members", "Volume", "Duration");

		// loop through conf list
		while (conf)
		{
			duration = (int)(ast_tvdiff_ms(ast_tvnow(),conf->time_entered) / 1000);
			snprintf(duration_str, 10, "%02d:%02d:%02d",  duration / 3600, (duration % 3600) / 60, duration % 60);
			ast_cli(fd, "%-20.20s %-20d %-20d %-20.20s\n", conf->name, conf->membercount, conf->volume, duration_str);
			conf = conf->next;
		}

		// release mutex
		ast_mutex_unlock(&conflist_lock);
	}
}

void list_members(int fd, const char *name)
{
	ast_conf_member *member;
	char volume_str[10];
	char spy_str[10];
	int duration;
	char duration_str[10];

        // any conferences?
	if (conflist)
	{

		// acquire mutex
		ast_mutex_lock(&conflist_lock);

		ast_conference *conf = conflist;

		// loop through conf list
		while (conf)
		{
			if (!strcasecmp((const char*)&(conf->name), name))
			{
				// acquire conference lock
				ast_rwlock_rdlock(&conf->lock);

				// print the header
				ast_cli(fd, "%s:\n%-20.20s %-20.20s %-20.20s %-20.20s %-20.20s %-20.20s %-80.20s\n", conf->name, "User #", "Flags", "Audio", "Volume", "Duration", "Spy", "Channel");
				// do the biz
				member = conf->memberlist;
				while (member)
				{
					snprintf(volume_str, 10, "%d:%d", member->talk_volume, member->listen_volume);
					if (member->spyee_channel_name && member->spy_partner)
						snprintf(spy_str, 10, "%d", member->spy_partner->conf_id);
					else
						strcpy(spy_str , "*");
					duration = (int)(ast_tvdiff_ms(ast_tvnow(),member->time_entered) / 1000);
					snprintf(duration_str, 10, "%02d:%02d:%02d",  duration / 3600, (duration % 3600) / 60, duration % 60);
					ast_cli(fd, "%-20d %-20.20s %-20.20s %-20.20s %-20.20s %-20.20s %-80s\n",
#if	ASTERISK_SRC_VERSION < 1100
					member->conf_id, member->flags, !member->mute_audio ? "Unmuted" : "Muted", volume_str, duration_str , spy_str, member->chan->name);
#else
					member->conf_id, member->flags, !member->mute_audio ? "Unmuted" : "Muted", volume_str, duration_str , spy_str, ast_channel_name(member->chan));
#endif
					member = member->next;
				}

				// release conference lock
				ast_rwlock_unlock(&conf->lock);

				break;
			}

			conf = conf->next;
		}

		// release mutex
		ast_mutex_unlock(&conflist_lock);
	}
}

void list_all(int fd)
{
	ast_conf_member *member;
	char volume_str[10];
	char spy_str[10];
	int duration;
	char duration_str[10];

        // any conferences?
	if (conflist)
	{
		// acquire mutex
		ast_mutex_lock(&conflist_lock);

		ast_conference *conf = conflist;

		// loop through conf list
		while (conf)
		{
			// acquire conference lock
			ast_rwlock_rdlock(&conf->lock);

			// print the header
			ast_cli(fd, "%s:\n%-20.20s %-20.20s %-20.20s %-20.20s %-20.20s %-20.20s %-80.20s\n", conf->name, "User #", "Flags", "Audio", "Volume", "Duration", "Spy", "Channel");
			// do the biz
			member = conf->memberlist;
			while (member)
			{
				snprintf(volume_str, 10, "%d:%d", member->talk_volume, member->listen_volume);
				if (member->spyee_channel_name && member->spy_partner)
					snprintf(spy_str, 10, "%d", member->spy_partner->conf_id);
				else
					strcpy(spy_str , "*");
				duration = (int)(ast_tvdiff_ms(ast_tvnow(),member->time_entered) / 1000);
				snprintf(duration_str, 10, "%02d:%02d:%02d",  duration / 3600, (duration % 3600) / 60, duration % 60);
				ast_cli(fd, "%-20d %-20.20s %-20.20s %-20.20s %-20.20s %-20.20s %-80s\n",
#if	ASTERISK_SRC_VERSION < 1100
				member->conf_id, member->flags, !member->mute_audio ? "Unmuted" : "Muted", volume_str, duration_str , spy_str, member->chan->name);
#else
				member->conf_id, member->flags, !member->mute_audio ? "Unmuted" : "Muted", volume_str, duration_str , spy_str, ast_channel_name(member->chan));
#endif
				member = member->next;
			}

			// release conference lock
			ast_rwlock_unlock(&conf->lock);

			conf = conf->next;
		}

		// release mutex
		ast_mutex_unlock(&conflist_lock);
	}
}
#ifdef	KICK_MEMBER
void kick_member(const char* confname, int user_id)
{
	ast_conf_member *member;

	// any conferences?
	if (conflist)
	{
		// acquire mutex
		ast_mutex_lock(&conflist_lock);

		ast_conference *conf = conflist;

		// loop through conf list
		while (conf)
		{
			if (!strcasecmp((const char*)&(conf->name), confname))
			{
				// do the biz
				ast_rwlock_rdlock(&conf->lock);
				member = conf->memberlist;
				while (member)
				{
					if (member->conf_id == user_id)
					{
						member->kick_flag = 1;
						ast_queue_frame(member->chan, &ast_null_frame);
					}
					member = member->next;
				}
				ast_rwlock_unlock(&conf->lock);
				break;
			}

			conf = conf->next;
		}

		// release mutex
		ast_mutex_unlock(&conflist_lock);
	}
}
#endif
void kick_all(void)
{
  ast_conf_member *member;

        // any conferences?
	if (conflist)
	{
		// acquire mutex
		ast_mutex_lock(&conflist_lock);

		ast_conference *conf = conflist;

		// loop through conf list
		while (conf)
		{
			// do the biz
			ast_rwlock_rdlock(&conf->lock);
			member = conf->memberlist;
			while (member)
			{
				member->kick_flag = 1;
				ast_queue_frame(member->chan, &ast_null_frame);
				member = member->next;
			}
			ast_rwlock_unlock(&conf->lock);

			conf = conf->next;
		}

		// release mutex
		ast_mutex_unlock(&conflist_lock);
	}

}
#ifdef	MUTE_MEMBER
void mute_member(const char* confname, int user_id)
{
  ast_conf_member *member;

        // any conferences?
	if (conflist)
	{
		// acquire mutex
		ast_mutex_lock(&conflist_lock);

		ast_conference *conf = conflist;

		// loop through conf list
		while (conf)
		{
			if (!strcasecmp((const char*)&(conf->name), confname))
			{
				// do the biz
				ast_rwlock_rdlock(&conf->lock);
				member = conf->memberlist;
				while (member)
				{
					if (member->conf_id == user_id)
					{
#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
						*(speaker_scoreboard + member->score_id) = '\x00';
#endif
						member->mute_audio = 1;
						manager_event(
								EVENT_FLAG_CONF,
								"ConferenceMemberMute",
								"Channel: %s\r\n",
#if	ASTERISK_SRC_VERSION < 1100
								member->chan->name
#else
								ast_channel_name(member->chan)
#endif
						);
					}
					member = member->next;
				}
				ast_rwlock_unlock(&conf->lock);
				break;
			}

			conf = conf->next;
		}

		// release mutex
		ast_mutex_unlock(&conflist_lock);
	}
}
#endif
void mute_conference(const char* confname)
{
	ast_conf_member *member;

        // any conferences?
	if (conflist)
	{
		// acquire mutex
		ast_mutex_lock(&conflist_lock);

		ast_conference *conf;

		// get conference
		if  ((conf = find_conf(confname)))
		{
			// do the biz
			ast_rwlock_rdlock(&conf->lock);
			member = conf->memberlist;
			while (member)
			  {
			    if (!member->ismoderator)
			      {
#if	defined(SPEAKER_SCOREBOARD) && defined(CACHE_CONTROL_BLOCKS)
					*(speaker_scoreboard + member->score_id) = '\x00';
#endif
				      member->mute_audio = 1;
			      }
			    member = member->next;
			  }
			ast_rwlock_unlock(&conf->lock);
		}

		// release mutex
		ast_mutex_unlock(&conflist_lock);

		manager_event(
			EVENT_FLAG_CONF,
			"ConferenceMute",
			"ConferenceName: %s\r\n",
			confname
		);
	}

}
#ifdef	UNMUTE_MEMBER
void unmute_member(const char* confname, int user_id)
{
  ast_conf_member *member;

        // any conferences?
	if (conflist)
	{
		// acquire mutex
		ast_mutex_lock(&conflist_lock);

		ast_conference *conf = conflist;

		// loop through conf list
		while (conf)
		{
			if (!strcasecmp((const char*)&(conf->name), confname))
			{
				// do the biz
				ast_rwlock_rdlock(&conf->lock);
				member = conf->memberlist;
				while (member)
				{
					if (member->conf_id == user_id)
					{
						member->mute_audio = 0;
						manager_event(
								EVENT_FLAG_CONF,
								"ConferenceMemberUnmute",
								"Channel: %s\r\n",
#if	ASTERISK_SRC_VERSION < 1100
								member->chan->name
#else
								ast_channel_name(member->chan)
#endif
						);
					}
					member = member->next;
				  }
				ast_rwlock_unlock(&conf->lock);
				break;
			}

			conf = conf->next;
		}

		// release mutex
		ast_mutex_unlock(&conflist_lock);
	}
}
#endif
void unmute_conference(const char* confname)
{
	ast_conf_member *member;

        // any conferences?
	if (conflist)
	{
		// acquire mutex
		ast_mutex_lock(&conflist_lock);

		ast_conference *conf;

		// get conference
		if ((conf = find_conf(confname)))
		{
			// do the biz
			ast_rwlock_rdlock(&conf->lock);
			member = conf->memberlist;
			while (member)
			  {
			    if (!member->ismoderator)
			      {
				      member->mute_audio = 0;
			      }
			    member = member->next;
			  }
			ast_rwlock_unlock(&conf->lock);
		}

		// release mutex
		ast_mutex_unlock(&conflist_lock);

		manager_event(
			EVENT_FLAG_CONF,
			"ConferenceUnmute",
			"ConferenceName: %s\r\n",
			confname
		);
	}
}

ast_conf_member *find_member(const char *chan)
{
	ast_conf_member *member;
	struct channel_bucket *bucket = &(channel_table[hash(chan) % CHANNEL_TABLE_SIZE]);

	AST_LIST_LOCK(bucket);

	AST_LIST_TRAVERSE(bucket, member, hash_entry)
#if	ASTERISK_SRC_VERSION < 1100
		if (!strcmp(member->chan->name, chan)) {
#else
		if (!strcmp(ast_channel_name(member->chan), chan)) {
#endif
			ast_mutex_lock(&member->lock);
			member->use_count++;

			break;
		}

	AST_LIST_UNLOCK(bucket);

	return member;
}

#if	ASTERISK_SRC_VERSION == 104 || ASTERISK_SRC_VERSION == 106
void play_sound_channel(int fd, const char *channel, char **file, int mute, int tone, int n)
#else
void play_sound_channel(int fd, const char *channel, const char * const *file, int mute, int tone, int n)
#endif
{
	ast_conf_member *member;
	ast_conf_soundq *newsound;
	ast_conf_soundq **q;

	if ((member = find_member(channel)))
	{
#if	ASTERISK_SRC_VERSION < 1100
		if (!member->norecv_audio && !ast_test_flag(member->chan, AST_FLAG_MOH)
#else
		if (!member->norecv_audio && !ast_test_flag(ast_channel_flags(member->chan), AST_FLAG_MOH)
#endif
				&& (!tone || !member->soundq))
		{
			while (n-- > 0) {
				if (!(newsound = ast_calloc(1, sizeof(ast_conf_soundq))))
					break;

				ast_copy_string(newsound->name, *file, sizeof(newsound->name));

				// append sound to the end of the list.
				for (q=&member->soundq; *q; q = &((*q)->next));
				*q = newsound;

				file++;
			}

			member->muted = mute;

		}
		if (!--member->use_count && member->delete_flag)
			ast_cond_signal(&member->delete_var);
		ast_mutex_unlock(&member->lock);
	}
}

void stop_sound_channel(int fd, const char *channel)
{
	ast_conf_member *member;
	ast_conf_soundq *sound;
	ast_conf_soundq *next;

	if ((member = find_member(channel)))
	{
		// clear all sounds
		sound = member->soundq;

		while (sound)
		{
			next = sound->next;
			sound->stopped = 1;
			sound = next;
		}

			member->muted = 0;

		if (!--member->use_count && member->delete_flag)
			ast_cond_signal(&member->delete_var);
		ast_mutex_unlock(&member->lock);
	}
}

void start_moh_channel(int fd, const char *channel)
{
	ast_conf_member *member;

	if ((member = find_member(channel)))
	{
		if (!member->norecv_audio)
			{
			// clear all sounds
			ast_conf_soundq *next;
			ast_conf_soundq *sound = member->soundq;

			while (sound)
			{
				next = sound->next;
				sound->stopped = 1;
				sound = next;
			}

			member->muted = 1;
			member->ready_for_outgoing = 0;

			struct ast_frame *f;
			while ((f = get_outgoing_frame(member)))
			{
				ast_frfree(f);
			}

			ast_moh_start(member->chan, NULL, NULL);
		}

		if (!--member->use_count && member->delete_flag)
			ast_cond_signal(&member->delete_var);
		ast_mutex_unlock(&member->lock);
	}
}

void stop_moh_channel(int fd, const char *channel)
{
	ast_conf_member *member;

	if ((member = find_member(channel)))
	{
		if (!member->norecv_audio)
		{
			member->muted = 0;
			member->ready_for_outgoing = 1;

			ast_moh_stop(member->chan);
		}

		if (!--member->use_count && member->delete_flag)
			ast_cond_signal(&member->delete_var);
		ast_mutex_unlock(&member->lock);
	}
}

void start_moh_member(int fd, ast_conf_member* member)
{
	ast_mutex_lock(&member->lock);

	if (!member->norecv_audio)
	{
		// clear all sounds
		ast_conf_soundq *next;
		ast_conf_soundq *sound = member->soundq;

		while (sound)
		{
			next = sound->next;
			sound->stopped = 1;
			sound = next;
		}

		member->muted = 1;
		member->ready_for_outgoing = 0;

		struct ast_frame *f;
		while ((f = get_outgoing_frame(member)))
		{
			ast_frfree(f);
		}

		ast_moh_start(member->chan, NULL, NULL);
	}

	ast_mutex_unlock(&member->lock);
}

void stop_moh_member(int fd, ast_conf_member* member)
{
	ast_mutex_lock(&member->lock);

	if (!member->norecv_audio)
	{
		member->muted = 0;
		member->ready_for_outgoing = 1;

		ast_moh_stop(member->chan);
	}

	ast_mutex_unlock(&member->lock);
}

void talk_volume_channel(int fd, const char *channel, int up)
{
	ast_conf_member *member;

	if ((member = find_member(channel)))
	{
		up ? member->talk_volume++ : member->talk_volume--;

		if (!--member->use_count && member->delete_flag)
			ast_cond_signal(&member->delete_var);
		ast_mutex_unlock(&member->lock);
	}
}

void listen_volume_channel(int fd, const char *channel, int up)
{
	ast_conf_member *member;

	if ((member = find_member(channel)))
	{
		up ? member->listen_volume++ : member->listen_volume--;

		if (!--member->use_count && member->delete_flag)
			ast_cond_signal(&member->delete_var);
		ast_mutex_unlock(&member->lock);
	}
}

void volume(int fd, const char *conference, int up)
{
	ast_conference *conf;

	// acquire the conference list lock
	ast_mutex_lock(&conflist_lock);

	if ((conf = find_conf(conference)))
	{
		// acquire the conference lock
		ast_rwlock_wrlock(&conf->lock);

		// adjust volume
		up ? conf->volume++ : conf->volume--;

		// release the conference lock
		ast_rwlock_unlock(&conf->lock);
	}

	// release the conference list lock
	ast_mutex_unlock(&conflist_lock);
}

int hash(const char *name)
{
	int i = 0, h = 0, g;
	while (name[i])
	{
		h = (h << 4) + name[i++];
		if ((g = h & 0xF0000000))
			h ^= g >> 24;
		h &= ~g;
	}
	return h;
}

#if	ASTERISK_SRC_VERSION == 104
int count_exec(struct ast_channel* chan, void* data)
#else
int count_exec(struct ast_channel* chan, const char* data)
#endif
{
	int res = 0;
	ast_conference *conf;
	int count;
	char *localdata;
	char val[80] = "0"; 
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(confno);
		AST_APP_ARG(varname);
	);
	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "ConferenceCount requires an argument (conference number)\n");
		return -1;
	}

	if (!(localdata = ast_strdupa(data))) {
		return -1;
	}

	AST_STANDARD_APP_ARGS(args, localdata);
	
	ast_mutex_lock(&conflist_lock);

	conf = find_conf(args.confno);

	if (conf) {
		count = conf->membercount;
	} else
		count = 0;

	ast_mutex_unlock(&conflist_lock);

	if (!ast_strlen_zero(args.varname)){
		snprintf(val, sizeof(val), "%d",count);
		pbx_builtin_setvar_helper(chan, args.varname, val);
	} else {
#if	ASTERISK_SRC_VERSION < 1100
		if (chan->_state != AST_STATE_UP)
#else
		if (ast_channel_state(chan) != AST_STATE_UP)
#endif
			ast_answer(chan);
#if	ASTERISK_SRC_VERSION < 1100
		res = ast_say_number(chan, count, "", chan->language, (char *) NULL);
#else
		res = ast_say_number(chan, count, "", ast_channel_language(chan), (char *) NULL);
#endif
	}
	return res;
}
