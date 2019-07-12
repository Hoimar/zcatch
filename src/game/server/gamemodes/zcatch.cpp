/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <engine/shared/config.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>
#include "zcatch.h"

CGameControllerZCATCH::CGameControllerZCATCH(CGameContext *pGameServer) : IGameController(pGameServer)
{
	m_pGameType = "zCatch/dev";
	m_GameFlags = GAMEFLAG_SURVIVAL;

	m_PreviousIngamePlayerCount = 0;
	m_IngamePlayerCount = 0;

	m_ForcedEndRound = false;
	
}

 void CGameControllerZCATCH::EndRound()
 {
	 // don't do anything if we just switched from
	 // warmup to zCatch or from zCatch to warmup
	if(m_PreviousIngamePlayerCount != m_IngamePlayerCount)
	{
		return;
	}

	// release all players at the end of the round.
	float alivePercentage = 0.0f, caughtPercentage = 0.0f;
	int totalTicksPlayed = 0;
	CPlayer *player = nullptr;
	char aBuf[256];
	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		player = GameServer()->m_apPlayers[i];
		if(player)
		{
			// calculate some small player statistict
			totalTicksPlayed = player->m_TicksAlive + player->m_TicksCaught;
			if (totalTicksPlayed > 0)
			{
				alivePercentage = (player->m_TicksAlive / totalTicksPlayed) * 100.0f;
				caughtPercentage = (player->m_TicksCaught / totalTicksPlayed) * 100.0f;

				// send them to the player
				str_format(aBuf, sizeof(aBuf), "Ingame: %.2f%% Spectating: %.2f%% ", alivePercentage, caughtPercentage);
				GameServer()->SendServerMessage(i, aBuf);
			}

			// do cleanup
			GameServer()->m_apPlayers[i]->ReleaseAllCaughtPlayers(CPlayer::REASON_EVERYONE_RELEASED);
			GameServer()->m_apPlayers[i]->ResetStatistics();

			player = nullptr;
		}
	}

	// Change game state
	IGameController::EndRound();
 }

// game
void CGameControllerZCATCH::DoWincheckRound()
{
	// forced end round to fallback to warmup
	if (m_ForcedEndRound)
	{
		m_ForcedEndRound = false;
		EndRound();
	}
	else if(m_GameInfo.m_TimeLimit > 0 && (Server()->Tick()-m_GameStartTick) >= m_GameInfo.m_TimeLimit*Server()->TickSpeed()*60)
	{
		// check for time based win
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->IsNotCaught())
				GameServer()->m_apPlayers[i]->m_Score++;
		}

		EndRound();
	}
	else
	{
		// check for survival win
		CPlayer *pAlivePlayer = 0;
		
		int alivePlayerCount = 0;
		m_PreviousIngamePlayerCount = m_IngamePlayerCount;
		m_IngamePlayerCount = 0;

		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(GameServer()->m_apPlayers[i])
			{
				if (GameServer()->m_apPlayers[i]->IsNotCaught())
				{
					++alivePlayerCount;
					pAlivePlayer = GameServer()->m_apPlayers[i];
				}
				
				if (GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
				{
					m_IngamePlayerCount++;
				}	
			}
		}

		if(alivePlayerCount == 0)
		{
			// no winner
			EndRound();
		}	
		else if(alivePlayerCount == 1)	// 1 winner
		{
			pAlivePlayer->m_Score++;
			
			// Inform everyone about the winner.
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "'%s' won the round!", Server()->ClientName(pAlivePlayer->GetCID()));
			GameServer()->SendServerMessage(-1, aBuf);

			EndRound();
		}
		else if(m_PreviousIngamePlayerCount >= g_Config.m_SvPlayersToStartRound && m_IngamePlayerCount < g_Config.m_SvPlayersToStartRound)
		{
			// Switching back to warmup!
			dbg_msg("DEBUG", "Switching back to warm up mode.");
			EndRound();
		}
	}
}

void CGameControllerZCATCH::OnCharacterSpawn(class CCharacter *pChr)
{	
	CCharacter& character = (*pChr);
	CPlayer& player = (*pChr->GetPlayer());

	// Gets weapons in Character::Spawn

	// gets health here.
	character.IncreaseHealth(10);

	// if we spawn, we should not have anyone caught from previous rounds.
	// or weird post mortem kills.
	player.ReleaseAllCaughtPlayers();

	// spawns -> joins spec
	if (player.GetWantsToJoinSpectators())
	{
		DoTeamChange(pChr->GetPlayer(), TEAM_SPECTATORS);
		player.ResetWantsToJoinSpectators();
		return;
	}

	// send broadcast if player stays ingame
	char aBuf[64];
	int enemiesLeft = player.UpdatePlayersLeftToCatch();
	if (enemiesLeft > 0)
	{
		str_format(aBuf, sizeof(aBuf), "%d enem%s left", enemiesLeft, enemiesLeft == 1 ? "y" : "ies");
		GameServer()->SendBroadcast(aBuf, player.GetCID());
	}
	else
	{
		GameServer()->SendBroadcast("", player.GetCID());
	}
	
	// If the player spawns, the releasing player's
	// enemiesLeft counter needs to be updated as well.
	RefreshBroadcast();
	UpdateSkinsOf({player.GetCID()});
}

void CGameControllerZCATCH::OnPlayerConnect(class CPlayer *pPlayer)
{
	CPlayer& player = (*pPlayer);
	int gamestate = GetGameState();
	
	// warmup
	if (gamestate == IGS_WARMUP_GAME || gamestate == IGS_WARMUP_USER)
	{	
		if (g_Config.m_Debug)
		{
			dbg_msg("DEBUG", "Player %d joined the game.", player.GetCID());
		}
		
		UpdateSkinsOf({player.GetCID()});
		IGameController::OnPlayerConnect(pPlayer);
		return;
	}

	// actual game running
	CPlayer *pDominatingPlayer = nullptr;

	// find player with most kills
	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		// if we join, we cannot be the dominating player
		// skip ourself.
		if (player.GetCID() == i)
		{
			continue;
		}

		if (GameServer()->m_apPlayers[i])
		{
			if (!pDominatingPlayer)
			{
				pDominatingPlayer = GameServer()->m_apPlayers[i];
			}
			else if (GameServer()->m_apPlayers[i]->GetNumCaughtPlayers() > pDominatingPlayer->GetNumCaughtPlayers())
			{
				pDominatingPlayer = GameServer()->m_apPlayers[i];
			}
		}
	}

	// if the dominating player has nobody caught, we don't want them to
	if (pDominatingPlayer && 
		pDominatingPlayer->GetNumCaughtPlayers() + pDominatingPlayer->GetNumLeftCaughtPlayers() > 0)
	{
		pDominatingPlayer->CatchPlayer(player.GetCID(), CPlayer::REASON_PLAYER_JOINED);
	}
	else if (pDominatingPlayer && 
			pDominatingPlayer->GetNumCaughtPlayers() + pDominatingPlayer->GetNumLeftCaughtPlayers() == 0)
	{
		if (m_IngamePlayerCount > g_Config.m_SvPlayersToStartRound 
			&& m_IngamePlayerCount == m_PreviousIngamePlayerCount)
		{
			// if the player joins & nobody has caught anybody 
			// at that exact moment, when a round is running
			// the player directly joins the game
			
			// we don't want this message to be displayed in any other case
			player.BeReleased(CPlayer::REASON_PLAYER_JOINED);
		}
		else if(m_IngamePlayerCount == g_Config.m_SvPlayersToStartRound
				&& m_IngamePlayerCount != m_PreviousIngamePlayerCount)
		{
			// no chat announcements
			// when switching game modes/states
			player.BeReleased();
		}
	}
	
	// needed to do the spawning stuff.
	IGameController::OnPlayerConnect(pPlayer);
}


void CGameControllerZCATCH::OnPlayerDisconnect(class CPlayer *pPlayer)
{
	CPlayer& player = (*pPlayer);
	
	// if player was ingame and not in spec when leaving
	if (player.GetTeam() != TEAM_SPECTATORS)
	{
		if (player.IsNotCaught())
		{
			// player being released 
			player.ReleaseAllCaughtPlayers(CPlayer::REASON_PLAYER_LEFT);

		}
		else if (player.IsCaught())
		{
			// remove leaving player from caught list 
			player.BeSetFree(CPlayer::REASON_PLAYER_LEFT);
		}
		
		// I leave, decrease number of ingame players
		m_IngamePlayerCount--;
		

		// not enough players to play a round
		if (m_IngamePlayerCount < g_Config.m_SvPlayersToStartRound)
		{
			// end round and do warmup again.
			m_ForcedEndRound = true;
		}
	}

	// needed to do the disconnect handling.
	IGameController::OnPlayerDisconnect(pPlayer);
}


int CGameControllerZCATCH::OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon)
{
	CPlayer& victim = (*pVictim->GetPlayer());
	CPlayer& killer = (*pKiller);
	int gamestate = GetGameState();

	// warmup
	if (gamestate == IGS_WARMUP_GAME || gamestate == IGS_WARMUP_USER)
	{
		// any kind of warmup
		if (g_Config.m_Debug)
		{
			dbg_msg("DEBUG", "Warmup, player %d killed by %d.", victim.GetCID(), killer.GetCID());
		}
		
		// simply die & respawn
		return IGameController::OnCharacterDeath(pVictim, pKiller, Weapon);
	}
	
	// actual game
	/**
	 * CPlayer::KillCharacter -> CCharacter::Die -> IGameController::OnCharacterDeath
	 * the cases, where victim.GetNumCaughtPlayers() > 0 are handled in 
	 * CPlayer::KillCharacter, prior to these cases, where it is possible to prevent 
	 * the character from actually being killed, when releasing a player, 
	 * that was killed by mistake.
	 */
	if(victim.GetCID() != killer.GetCID() && Weapon >= 0)
	{
		// someone killed me with a real weapon
		killer.CatchPlayer(victim.GetCID());
	}
	else if(Weapon < 0 && victim.GetCID() == killer.GetCID())
	{
		switch (Weapon)
		{
		case WEAPON_WORLD: // death tiles etc.
			victim.ReleaseAllCaughtPlayers(CPlayer::REASON_PLAYER_FAILED);
			break;
		case WEAPON_SELF: // suicide
			// here we catch literally the suicides, not the releases
			GameServer()->SendServerMessage(victim.GetCID(), "Was it really necessary to kill youself?");
			break;
		case WEAPON_GAME: // team change, etc.
			if (g_Config.m_Debug)
			{
				dbg_msg("DEBUG", "ID: %d was killed by the game.", victim.GetCID());
			}
			break;
		default:
			break;
		}
	}

	// update broadcast for killer and victim
	char aBuf[64];

	// send broadcast update to killer
	int enemiesLeft = killer.UpdatePlayersLeftToCatch();
	if (enemiesLeft > 0)
	{
		str_format(aBuf, sizeof(aBuf), "%d enem%s left", enemiesLeft, enemiesLeft == 1 ? "y":"ies");
		GameServer()->SendBroadcast(aBuf, killer.GetCID());
	}
	else
	{
		GameServer()->SendBroadcast("", killer.GetCID());
	}
	
	// send broadcast update to victim
	enemiesLeft = victim.UpdatePlayersLeftToCatch();
	if (enemiesLeft > 0)
	{
		str_format(aBuf, sizeof(aBuf), "%d enem%s left", enemiesLeft, enemiesLeft == 1 ? "y":"ies");
		GameServer()->SendBroadcast(aBuf, victim.GetCID());
	}
	else
	{
		GameServer()->SendBroadcast("", victim.GetCID());
	}

	// update colors of both players
	UpdateSkinsOf({victim.GetCID(), killer.GetCID()});

	// vanilla handling
	// do scoreing
	if(!pKiller || Weapon == WEAPON_GAME)
		return 0;
	if(pKiller == pVictim->GetPlayer())
	{
		// suicide or falling out of the map
		if (Weapon == WEAPON_WORLD || (Weapon == WEAPON_SELF && victim.GetNumCaughtPlayers() == 0))
		{
			pVictim->GetPlayer()->m_Score -= g_Config.m_SvSuicidePenalty; 
		}		
	}
		
	if(Weapon == WEAPON_SELF)
	{
		// respawn in 3 seconds
		pVictim->GetPlayer()->m_RespawnTick = Server()->Tick()+Server()->TickSpeed()*3.0f;
	}

	// update spectator modes for dead players in survival
	if(m_GameFlags&GAMEFLAG_SURVIVAL)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
			if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->m_DeadSpecMode)
				GameServer()->m_apPlayers[i]->UpdateDeadSpecMode();
	}
	
	return 0;
}


void CGameControllerZCATCH::Tick()
{
	// Broadcast Refresh is only needed for solely 
	// keeping the broadcast visible, but not to push
	// actual updates.
	int nineSeconds = Server()->TickSpeed() * 9;
	if (Server()->Tick() % nineSeconds == 0)
	{
		// The broadcast is according to heinrich5991 kept shown
		// by the client for 10 seconds.
		RefreshBroadcast();
	}
	
	IGameController::Tick();
}


void CGameControllerZCATCH::DoTeamChange(class CPlayer *pPlayer, int Team, bool DoChatMsg)
{
	CPlayer& player = (*pPlayer);

	// toggle state, whether player wants or doesn't want to joint spec.
	if (player.IsCaught() && Team == TEAM_SPECTATORS)
	{
		// player wants to join spec, but is caught.

		char aBuf[256];
		if (player.GetWantsToJoinSpectators())
		{	
			str_format(aBuf, sizeof(aBuf), "You will join the game once '%s' dies.", Server()->ClientName(player.GetIDCaughtBy()));

			player.ResetWantsToJoinSpectators();
		}
		else
		{
			str_format(aBuf, sizeof(aBuf), "You will join the spectators once '%s' dies.", Server()->ClientName(player.GetIDCaughtBy()));

			player.SetWantsToJoinSpectators();
		}

		GameServer()->SendServerMessage(player.GetCID(), aBuf);
		// do not change team, while you are caught.
		return;
	}
	else if(player.IsNotCaught() && player.GetNumCaughtPlayers() > 0 && Team == TEAM_SPECTATORS)
	{
		player.ReleaseAllCaughtPlayers(CPlayer::REASON_PLAYER_JOINED_SPEC);
	}
	else if(player.GetTeam() == TEAM_SPECTATORS && Team != TEAM_SPECTATORS)
	{
		// players joins the game after being in spec

		// do vanilla respawn stuff
		IGameController::DoTeamChange(pPlayer, Team, DoChatMsg);

		// force player to spawn
		player.BeReleased(CPlayer::REASON_PLAYER_JOINED_GAME_AGAIN);
		return;
	}
	
	IGameController::DoTeamChange(pPlayer, Team, DoChatMsg);

}


void CGameControllerZCATCH::OnPlayerInfoChange(class CPlayer *pPlayer)
{
	// nothing
}


void CGameControllerZCATCH::RefreshBroadcast()
{
	// basically only print the stored values of each player O(n)
	// no updates are handled in here. Those should be handled
	// at their occurrence place.
	CPlayer *pTmpPlayer = nullptr;
	char aBuf[64];
	int enemiesLeft = 0;

	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		pTmpPlayer = GameServer()->m_apPlayers[i];

		// spectating players should not receive any broadcasts.
		if (pTmpPlayer && pTmpPlayer->GetTeam() != TEAM_SPECTATORS)
		{
			enemiesLeft = pTmpPlayer->GetPlayersLeftToCatch();
			
			if (enemiesLeft > 0)
			{
				str_format(aBuf, sizeof(aBuf), "%d enem%s left", enemiesLeft, enemiesLeft == 1 ? "y":"ies");
				GameServer()->SendBroadcast(aBuf, i);
			}
			else
			{
				GameServer()->SendBroadcast("", i);
			}	
		}
		
		pTmpPlayer = nullptr;
	}	
}


void CGameControllerZCATCH::UpdateSkinsOf(std::initializer_list<int> IDs)
{
	for (int toID = 0; toID < MAX_CLIENTS; toID++)
	{
		if (GameServer()->m_apPlayers[toID])
		{
			// send skin update message of id to everyone
			for (int id : IDs)
			{
				GameServer()->SendSkinChange(toID, id);
			}
		}
	}
}




