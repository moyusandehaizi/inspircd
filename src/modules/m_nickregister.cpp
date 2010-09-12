/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2010 InspIRCd Development Team
 * See: http://wiki.inspircd.org/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"
#include "account.h"
#include "nickregister.h"

static dynamic_reference<AccountProvider> accounts("account");

struct NickData
{
	std::string account;
	time_t ts;
	time_t last_used;
	NickData(const std::string& a, time_t t, time_t l)
		: account(a), ts(t), last_used(l) {}
	void update(const NickData& incoming)
	{
		if (ts == incoming.ts && account == incoming.account)
		{
			// match, just update last_used
			if (last_used < incoming.last_used)
				last_used = incoming.last_used;
		}
		else if (ts > incoming.ts || (ts == incoming.ts && account < incoming.account))
		{
			// we got replaced by TS collision
			*this = incoming;
		}
		else
		{
			// we won the TS collision; discard
		}
	}
};

/** Nick mapped to information */
typedef std::map<std::string, NickData> NickMap;
/** Account mapped to nick(s) */
typedef std::multimap<std::string, std::string> OwnerMap;

class RegDB : public NickRegistrationProvider
{
 public:
	NickMap nickinfo;
	OwnerMap nicksowned;
	time_t expiry;

	RegDB(Module* parent) : NickRegistrationProvider(parent) {}

	std::vector<std::string> GetNicks(const std::string& useraccount)
	{
		std::vector<std::string> rv;
		std::pair<OwnerMap::iterator, OwnerMap::iterator> range = nicksowned.equal_range(useraccount);
		for(OwnerMap::iterator i = range.first; i != range.second; i++)
			rv.push_back(i->second);
		return rv;
	}

	std::string GetOwner(const std::string& nick)
	{
		NickMap::iterator it = nickinfo.find(nick);
		if (it == nickinfo.end())
			return "";
		return it->second.account;
	}

	void UpdateLastUse(const std::string& nick)
	{
		NickMap::iterator it = nickinfo.find(nick);
		if (it != nickinfo.end())
			it->second.last_used = ServerInstance->Time();
	}

	void SetOwner(const std::string& nick, const std::string& acct)
	{
		time_t now = ServerInstance->Time();
		SetOwner(nick, acct, now, now);
		parameterlist params;
		params.push_back("*");
		params.push_back("NICKREGISTER");
		params.push_back(nick);
		params.push_back(acct);
		params.push_back(ConvToStr(now));
		params.push_back(ConvToStr(now));
		ServerInstance->PI->SendEncapsulatedData(params);
	}

	void SetOwner(const std::string& nick, const std::string& regaccount, time_t ts, time_t luts)
	{
		NickData value(regaccount, ts, luts);
		NickMap::iterator it = nickinfo.find(nick);
		if (it == nickinfo.end())
		{
			if (regaccount != "-")
			{
				nickinfo.insert(std::make_pair(nick, value));
				nicksowned.insert(std::make_pair(value.account, nick));
			}
		}
		else
		{
			std::string oldowner = it->second.account;
			it->second.update(value);
			if (regaccount == "-" || it->second.account != oldowner)
			{
				OwnerMap::iterator rev = nicksowned.lower_bound(oldowner);
				while (rev != nicksowned.end() && rev->first == oldowner && rev->second != nick)
					rev++;
				if (rev != nicksowned.end() && rev->first == oldowner)
					nicksowned.erase(rev);
				if (regaccount == "-")
					nickinfo.erase(it);
				else
					nicksowned.insert(std::make_pair(it->second.account, nick));
			}
		}
	}

	void Clean()
	{
		NickMap::iterator i = nickinfo.begin();
		time_t cutoff = ServerInstance->Time() - expiry;
		while (i != nickinfo.end())
		{
			NickMap::iterator curr = i++;
			if (curr->second.last_used < cutoff)
			{
				OwnerMap::iterator rev = nicksowned.lower_bound(curr->second.account);
				while (rev != nicksowned.end() && rev->first == curr->second.account && rev->second != curr->first)
					rev++;
				if (rev != nicksowned.end() && rev->first == curr->second.account)
					nicksowned.erase(rev);
				nickinfo.erase(curr);
			}
		}
	}
};

class CommandRegisterNick : public Command
{
 public:
	RegDB db;

	int maxreg;
	CommandRegisterNick(Module* parent) : Command(parent, "NICKREGISTER"), db(parent), maxreg(5)
	{
		syntax = "[[<nick>] <account>|-]";
	}

	inline void setflags(char f) { flags_needed = f; }

	CmdResult Handle(const std::vector<std::string>& parameters, User* user)
	{
		std::string nick = parameters.size() > 1 ? parameters[0] : user->nick;
		std::string useraccount = accounts ? accounts->GetAccountName(user) : "";
		std::string regaccount =
			parameters.size() > 1 ? parameters[1] :
			parameters.size() > 0 ? parameters[0] : useraccount;

		// TODO convert to numerics for errors
		if (!user->HasPrivPermission("nicks/set-registration", false))
		{
			// users can only register their own nick to their own account
			if (irc::string(nick) != irc::string(user->nick))
			{
				user->WriteServ("NOTICE %s :You can only register your own nick", user->nick.c_str());
				return CMD_FAILURE;
			}
			if (regaccount != "-")
			{
				if (regaccount != useraccount)
				{
					user->WriteServ("NOTICE %s :You can only register to your own account", user->nick.c_str());
					return CMD_FAILURE;
				}
				// and they can't register more than 5 per account (or the conf'd max)
				int count = db.GetNicks(useraccount).size();
				if (count > maxreg)
				{
					user->WriteServ("NOTICE %s :You can only register %d nicks", user->nick.c_str(), maxreg);
					return CMD_FAILURE;
				}
			}
		}
		if (IS_LOCAL(user) && !ServerInstance->IsNick(nick.c_str(), ServerInstance->Config->Limits.NickMax))
		{
			user->WriteServ("NOTICE %s :Not a valid nick", user->nick.c_str());
			return CMD_FAILURE;
		}

		time_t ts = ServerInstance->Time();
		time_t luts = ServerInstance->Time();
		if (IS_SERVER(user) && parameters.size() > 3)
		{
			ts = atoi(parameters[2].c_str());
			luts = atoi(parameters[3].c_str());
		}
		else
		{
			// TODO allow this to be done by changing API
			std::vector<std::string>& pmod = const_cast<std::vector<std::string>&>(parameters);
			pmod.resize(4);
			pmod[0] = nick;
			pmod[1] = regaccount;
			pmod[2] = ConvToStr(ts);
			pmod[3] = ConvToStr(luts);
		}

		db.SetOwner(nick, regaccount, ts, luts);
		if (IS_LOCAL(user))
			user->WriteServ("NOTICE %s :You have successfully %sregistered the nick %s",
				user->nick.c_str(), regaccount == "-" ? "un" : "", nick.c_str());
		return CMD_SUCCESS;
	}

	RouteDescriptor GetRouting(User* user, const std::vector<std::string>& parameters)
	{
		return ROUTE_OPT_BCAST;
	}
};

class ModuleNickRegister : public Module
{
 public:
	CommandRegisterNick cmd;

	ModuleNickRegister() : cmd(this) {}

	void ReadConfig(ConfigReadStatus& status)
	{
		ConfigTag* tag = status.GetTag("nickregister");
		std::string mode = tag->getString("mode", "services");
		if (mode == "services")
			cmd.setflags(FLAG_SERVERONLY);
		else if (mode == "opers")
			cmd.setflags('o');
		else if (mode == "users")
			cmd.setflags(0);
		else
			status.ReportError(tag, "<nickregister:mode> must be one of: services, opers, users");

		cmd.maxreg = tag->getInt("maxperaccount", 5);
		cmd.db.expiry = ServerInstance->Duration(tag->getString("expiretime", "21d"));
	}

	void init()
	{
		ServerInstance->Modules->AddService(cmd);
		ServerInstance->Modules->AddService(cmd.db);
		Implementation eventlist[] = {
			I_OnUserPreNick, I_OnCheckReady, I_OnSyncNetwork, I_OnUserQuit, I_OnGarbageCollect
		};
		ServerInstance->Modules->Attach(eventlist, this, sizeof(eventlist)/sizeof(Implementation));
	}

	ModResult OnUserPreNick(User* user, const std::string& nick)
	{
		// update timestamp on old nick
		cmd.db.UpdateLastUse(user->nick);

		if (ServerInstance->NICKForced.get(user))
			return MOD_RES_PASSTHRU;

		// check the new nick
		std::string owner = cmd.db.GetOwner(nick);
		std::string acctname = accounts ? accounts->GetAccountName(user) : "";
		if (owner.empty() || owner == acctname)
			return MOD_RES_PASSTHRU;
		if (user->registered == REG_ALL)
		{
			user->WriteNumeric(433, "%s %s :You must be identified to the account '%s' to use this nick",
				user->nick.c_str(), nick.c_str(), owner.c_str());
			return MOD_RES_DENY;
		}
		else
		{
			// allow through to give things like SASL or SQLauth time to return before denying
			user->WriteNumeric(437, "%s %s :This nick requires you to identify to the account '%s'",
				user->nick.c_str(), nick.c_str(), owner.c_str());
			return MOD_RES_PASSTHRU;
		}
	}

	ModResult OnCheckReady(LocalUser* user)
	{
		std::string owner = cmd.db.GetOwner(user->nick);
		if (owner.empty())
			return MOD_RES_PASSTHRU;
		std::string acctname = accounts ? accounts->GetAccountName(user) : "";
		if (owner != acctname)
		{
			user->WriteNumeric(433, "%s %s :Nickname overruled: requires login to the account '%s'",
				user->nick.c_str(), user->nick.c_str(), owner.c_str());
			user->ChangeNick(user->uuid, true);
		}
		return MOD_RES_PASSTHRU;
	}

	void OnUserQuit(User* user, const std::string&, const std::string&)
	{
		cmd.db.UpdateLastUse(user->nick);
	}

	void OnGarbageCollect()
	{
		cmd.db.Clean();
	}

	void OnSyncNetwork(SyncTarget* target)
	{
		for(NickMap::iterator i = cmd.db.nickinfo.begin(); i != cmd.db.nickinfo.end(); i++)
		{
			parameterlist params;
			params.push_back(i->first);
			params.push_back(i->second.account);
			params.push_back(ConvToStr(i->second.ts));
			params.push_back(ConvToStr(i->second.last_used));
			target->SendEncap(cmd.name, params);
		}
	}

	void Prioritize()
	{
		// need to be after all modules that might deny the ready check
		ServerInstance->Modules->SetPriority(this, I_OnCheckReady, PRIORITY_LAST);
	}

	Version GetVersion()
	{
		return Version("Nick registration tracking", VF_VENDOR | VF_OPTCOMMON);
	}
};

MODULE_INIT(ModuleNickRegister)
