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

#ifndef _KONFERENCE_CONFERENCE_H
#define _KONFERENCE_CONFERENCE_H

//
// includes
//

#include "app_conference.h"
#include "member.h"

//
// defines
//

#define CONF_NAME_LEN 80

#define MAX_CONF_UID  128+22+1  // max system_name + two ints + terminating zero

//
// struct declarations
//

struct ast_conference
{
	// name
	char name[CONF_NAME_LEN + 1];
	
	// start time
	struct timeval time_entered;

	// unique conference ID
	char conf_uid[MAX_CONF_UID];

	// moderator count
	unsigned short moderators;

	// conference listener frame
	conf_frame *listener_frame;

	// conference volume
	int volume;

	// single-linked list of members in conference
	ast_conf_member* memberlist;

	// pointer to last member in list
	ast_conf_member* memberlast;

	int membercount;
        int id_count;

	// conference data lock
	ast_rwlock_t lock;

	// pointers to conference in doubly-linked list
	ast_conference* next;
	ast_conference* prev;

	// pointer to conference's bucket list head
	struct conference_bucket *bucket;
	// list entry for conference's bucket list
	AST_LIST_ENTRY(ast_conference) hash_entry;

	// pointer to translation paths
	struct ast_trans_pvt* from_slinear_paths[AC_SUPPORTED_FORMATS];

	// keep track of current delivery time
	struct timeval delivery_time;

	// listener mix buffer
#ifdef	VECTORS
	char listenerBuffer[AST_CONF_BUFFER_SIZE] __attribute((aligned(16)));
#else
	char listenerBuffer[AST_CONF_BUFFER_SIZE];
#endif
	// listener mix frames
	struct ast_frame *mixAstFrame;
	conf_frame *mixConfFrame;
};

//
// function declarations
//

int hash(const char *channel_name);

#if	ASTERISK_SRC_VERSION == 104
int count_exec(struct ast_channel* chan, void* data);
#else
int count_exec(struct ast_channel* chan, const char* data);
#endif

ast_conference* join_conference(ast_conf_member* member, char* conf_name, char* max_users_flag);

ast_conference* find_conf(const char* name);
// Find member, locked if found.
ast_conf_member *find_member(const char *chan);

void queue_frame_for_listener(ast_conference* conf, ast_conf_member* member);
void queue_frame_for_speaker(ast_conference* conf, ast_conf_member* member);
void queue_silent_frame(ast_conference* conf, ast_conf_member* member);

void get_unison_event_server_node_variable(struct ast_channel *channel, char **varval, char *workspace, int wssize);

void remove_member(ast_conf_member* member, ast_conference* conf, char* conf_name);

// called by app_conference.c:load_module()
int init_conference(void);
// called by app_conference.c:unload_module()
void dealloc_conference(void);

// cli functions
void end_conference(const char *name);

void list_members(int fd, const char* name);
void list_conferences(int fd);
void list_all(int fd);

#ifdef	KICK_MEMBER
void kick_member(const char* confname, int user_id);
#endif
void kick_all(void);

#ifdef MUTE_MEMBER
void mute_member(const char* confname, int user_id);
#endif
#ifdef UNMUTE_MEMBER
void unmute_member(const char* confname, int user_id);
#endif

void mute_conference(const char* confname);
void unmute_conference(const char* confname);

#if	ASTERISK_SRC_VERSION == 104 || ASTERISK_SRC_VERSION == 106
void play_sound_channel(int fd, const char *channel, char **file, int mute, int tone, int n);
#else
void play_sound_channel(int fd, const char *channel, const char * const *file, int mute, int tone, int n);
#endif

void stop_sound_channel(int fd, const char *channel);

void start_moh_member(int fd, ast_conf_member* member);
void stop_moh_member(int fd, ast_conf_member* member);

void start_moh_channel(int fd, const char *channel);
void stop_moh_channel(int fd, const char *channel);

void talk_volume_channel(int fd, const char *channel, int up);
void listen_volume_channel(int fd, const char *channel, int up);

void volume(int fd, const char *conference, int up);

#endif
