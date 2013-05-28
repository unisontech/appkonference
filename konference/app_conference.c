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

#include "asterisk.h"

// SVN revision number, provided by make
#ifndef REVISION
#define REVISION "unknown"
#endif

static char *revision = REVISION;

ASTERISK_FILE_VERSION(__FILE__, REVISION)

#include "app_conference.h"
#include "conference.h"
#include "cli.h"

/*
 *
 * a conference has N threads, where N is the number of members
 *
 * each member thread reads frames from its channel adding them 
 * to its frame queue which is read by the conference thread
 *
 * the conference thread reads frames from each speaking members
 * queue, mixes them, and then queues them to the member threads
 *
 */

static char *app = "Konference";
static char *synopsis = "Channel Independent Conference";
static char *descrip = "Channel Independent Conference Application";

static char *app2 = "KonferenceCount";
static char *synopsis2 = "Channel Independent Conference Count";
static char *descrip2 = "Channel Independent Conference Count Application";

static int action_conferencelist(struct mansession *s, const struct message *m);

#if	ASTERISK_SRC_VERSION == 104 || ASTERISK_SRC_VERSION == 106
static int app_konference_main(struct ast_channel* chan, void* data)
#else
static int app_konference_main(struct ast_channel* chan, const char* data)
#endif
{
	int res;
	struct ast_module_user *u;

	u = ast_module_user_add(chan);

	// call member thread function
	res = member_exec(chan, data);

	ast_module_user_remove(u);

	return res;
}

#if	ASTERISK_SRC_VERSION == 104 || ASTERISK_SRC_VERSION == 106
static int app_konferencecount_main(struct ast_channel* chan, void* data)
#else
static int app_konferencecount_main(struct ast_channel* chan, const char* data)
#endif
{
	int res;
	struct ast_module_user *u;

	u = ast_module_user_add(chan);

	// call count thread function
	res = count_exec(chan, data);

	ast_module_user_remove(u);

	return res;
}

static int unload_module(void)
{
	int res = 0;

	ast_log(LOG_NOTICE, "Unloading app_konference module\n");

	ast_module_user_hangup_all();

	unregister_conference_cli();

	res |= ast_unregister_application(app);
	res |= ast_unregister_application(app2);

	dealloc_conference();

	return res;
}

static int load_module(void)
{
	int res = 0;

	ast_log(LOG_NOTICE, "Loading app_konference module revision=%s\n", revision);

	res |= init_conference();

	register_conference_cli();

	res |= ast_register_application(app, app_konference_main, synopsis, descrip);
	res |= ast_register_application(app2, app_konferencecount_main, synopsis2, descrip2);
	res |= ast_manager_register_xml("ConferenceList", EVENT_FLAG_REPORTING, action_conferencelist);

	return res;
}

static int action_conferencelist(struct mansession *s, const struct message *m)
{
	const char *actionid = astman_get_header(m, "ActionID");
	const char *conference = astman_get_header(m, "Conference");
	ast_conf_member *member;
	ast_conference *conf;

	char id_text[80] = "";

	if (!ast_strlen_zero(actionid)) {
		snprintf(id_text, sizeof(id_text), "ActionID: %s\r\n", actionid);
	}
	if (ast_strlen_zero(conference)) {
		astman_send_error(s, m, "No Conference name provided.");
		return 0;
	}

	conf = find_conf(conference);
	if (!conf) {
		astman_send_error(s, m, "No Conference by that name found.");
		return 0;
	}

	astman_send_listack(s, m, "Conference user list will follow", "start");

	// acquire the conference lock
	ast_rwlock_wrlock(&conf->lock);

	if (conf->membercount >0)
	{
		member = conf->memberlist;
		while (member) {
			astman_append(s,
					"Event: ConferenceList\r\n"
					"%s"
					"Conference: %s\r\n"
					"CallerIDNum: %s\r\n"
					"CallerIDName: %s\r\n"
					"Channel: %s\r\n"
					"Moderator: %s\r\n"
					"\r\n",
					id_text,
					conf->name,
#if	ASTERISK_SRC_VERSION < 1100
		member->chan->caller.id.number.str ? member->chan->caller.id.number.str : "unknown",
		member->chan->caller.id.name.str ? member->chan->caller.id.name.str: "unknown",
		member->chan->name,
#else
		S_COR(ast_channel_caller(member->chan)->id.number.valid, ast_channel_caller(member->chan)->id.number.str, "<unknown>"),
		S_COR(ast_channel_caller(member->chan)->id.name.valid, ast_channel_caller(member->chan)->id.name.str, "<no name>"),
		ast_channel_name(member->chan),
#endif
					member->ismoderator ? "Yes" : "No");
			member = member->next;
		}
	}

	// release the conference lock
	ast_rwlock_unlock(&conf->lock);

	astman_append(s,
	"Event: ConfbridgeListComplete\r\n"
	"EventList: Complete\r\n"
	"ListItems: %d\r\n"
	"%s"
	"\r\n", conf->membercount, id_text);

	return 0;
}

#define AST_MODULE "Konference"
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Channel Independent Conference Application",
		.load = load_module,
		.unload = unload_module,
);
#undef AST_MODULE
