/* Copyright (C) 1994-2003 Revolution Software Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * $Header$
 */

//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------
// SAVE_REST.CPP	save, restore & restart functions
//
// James 05feb97
//
// "Jesus Saves", but could he Restore or Restart? He can now...
//
//------------------------------------------------------------------------------------

//#include <direct.h> directx?
#include <stdio.h>
#include <stdlib.h>

#include "stdafx.h"
#include "driver/driver96.h"
#include "console.h"
#include "defs.h"
#include "function.h"		// for engine_logic, engine_graph, etc
#include "interpreter.h"	// for IR_CONT, etc
#include "layers.h"
#include "logic.h"
#include "memory.h"
#include "object.h"
#include "protocol.h"
#include "resman.h"
#include "router.h"
#include "save_rest.h"
#include "scroll.h"			// for Set_scrolling()
#include "sound.h"
#include "walker.h"

//------------------------------------------------------------------------------------
#define	MAX_FILENAME_LEN	128		// max length of a savegame filename, including full path

//------------------------------------------------------------------------------------
// local function prototypes

void GetPlayerStructures(void);		// James27feb97
void PutPlayerStructures(void);		// James27feb97

uint32 SaveData(uint16 slotNo, uint8 *buffer, uint32 bufferSize);
uint32 RestoreData(uint16 slotNo, uint8 *buffer, uint32 bufferSize);

uint32 CalcChecksum(uint8 *buffer, uint32 size);	// James04aug97

//------------------------------------------------------------------------------------

typedef	struct		// savegame file header		(James06feb97)
{
	uint32			checksum;							// sum of all bytes in file, excluding this uint32
	char			description[SAVE_DESCRIPTION_LEN];	// player's description of savegame
	uint32			varLength;							// length of global variables resource
	uint32			screenId;							// resource id of screen file
	uint32			runListId;							// resource id of run list
	uint32			feet_x;								// copy of this_screen.feet_x
	uint32			feet_y;								// copy of this_screen.feet_y
	uint32			music_id;							// copy of 'looping_music_id'
	_object_hub		player_hub;							// copy of player object's object_hub structure
	Object_logic	logic;								// copy of player character logic structure
	Object_graphic	graphic;							// copy of player character graphic structure
	Object_mega		mega;								// copy of player character mega structure
}
_savegameHeader;

// savegame consists of header & global variables resource

_savegameHeader header;		// global because easier to copy to/from player object structures

//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------
// SAVE GAME
//------------------------------------------------------------------------------------

uint32 SaveGame(uint16 slotNo, uint8 *desc)		// (James05feb97)
{
	mem		*saveBufferMem;
	uint32	bufferSize;
	uint32	errorCode;

	//------------------------------------------------------
	// allocate the savegame buffer

	bufferSize = FindBufferSize();
	saveBufferMem = Twalloc( bufferSize, MEM_locked, UID_savegame_buffer );

	FillSaveBuffer(saveBufferMem, bufferSize, desc);

	//------------------------------------------------------
	// save it (platform-specific)

	errorCode = SaveData( slotNo, saveBufferMem->ad, bufferSize );	// save the buffer

 	//------------------------------------------------------
	// free the buffer

	Free_mem( saveBufferMem );

 	//------------------------------------------------------

	return(errorCode);
}

//------------------------------------------------------------------------------------
// calculate size of required savegame buffer

uint32 FindBufferSize( void )
{
	return (sizeof(header) + res_man.Res_fetch_len(1));	// size of savegame header + size of global variables
}

//------------------------------------------------------------------------------------

void FillSaveBuffer(mem *buffer, uint32 size, uint8 *desc)
{
	uint8	*varsRes;

	//------------------------------------------------------
	// set up the header

	// 'checksum' gets filled in last of all
	sprintf(header.description, "%s", (char*)desc);					// player's description of savegame
	header.varLength	= res_man.Res_fetch_len(1);					// length of global variables resource
	header.screenId		= this_screen.background_layer_id;			// resource id of current screen file
	header.runListId	= LLogic.Return_run_list();					// resource id of current run-list
	header.feet_x		= this_screen.feet_x;						// those scroll position control things
	header.feet_y		= this_screen.feet_y;						//
	header.music_id		= looping_music_id;							// id of currently looping music (or zero)

	// object hub
	memcpy (&header.player_hub, res_man.Res_open(CUR_PLAYER_ID) + sizeof(_standardHeader), sizeof(_object_hub));
	res_man.Res_close(CUR_PLAYER_ID);

	// logic, graphic & mega structures
	GetPlayerStructures();											// copy the 4 essential player object structures into the header

	//------------------------------------------------------
	// copy the header to the buffer

	memcpy( buffer->ad, &header, sizeof(header) );					// copy the header to the savegame buffer

	//------------------------------------------------------
	// copy the global variables to the buffer

	varsRes = res_man.Res_open(1);									// open variables resource
	memcpy( buffer->ad + sizeof(header), varsRes, header.varLength );	// copy that to the buffer, following the header
 	res_man.Res_close(1);											// close variables resource

	//------------------------------------------------------
	// set the checksum & copy that to the buffer (James05aug97)

	header.checksum = CalcChecksum((buffer->ad)+sizeof(header.checksum), size-sizeof(header.checksum));
 	memcpy( buffer->ad, &header.checksum, sizeof(header.checksum) );					// copy the header to the savegame buffer

	//------------------------------------------------------
}

//------------------------------------------------------------------------------------

uint32 SaveData(uint16 slotNo, uint8 *buffer, uint32 bufferSize)
{
	char saveFileName[MAX_FILENAME_LEN];
	FILE *fp;
	uint32	itemsWritten;


//create saves directory just in case not there
	_mkdir("saves");


	sprintf(saveFileName, "saves\\savegame.%.3d", slotNo);	// construct filename

	fp = fopen(saveFileName, "wb");					// attempt to open file for writing

	if (fp==NULL)
	{
		return(SR_ERR_FILEOPEN);					// error: couldn't open file
	}
	else
	{
//		itemsWritten = fwrite(sourceAddress, size, count, fp);
		itemsWritten = fwrite(buffer, 1, bufferSize, fp);			// write the buffer
 		fclose(fp);									// close savegame file

		if (itemsWritten == bufferSize)		// if we successfully wrote it all
			return(SR_OK);					// buffer saved ok
		else
			return(SR_ERR_WRITEFAIL);		// write failed for some reason (could be hard drive full)
	}
}

//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------
// RESTORE GAME
//------------------------------------------------------------------------------------

uint32 RestoreGame(uint16 slotNo)		// (James05feb97)
{
	mem		*saveBufferMem;
	uint32	bufferSize;
	uint32	errorCode;

	//------------------------------------------------------
	// allocate the savegame buffer

	bufferSize = FindBufferSize();
	saveBufferMem = Twalloc( bufferSize, MEM_locked, UID_savegame_buffer );

	//------------------------------------------------------
	// read the savegame file into our buffer

	errorCode = RestoreData( slotNo, saveBufferMem->ad, bufferSize );	// load savegame into buffer

	//------------------------------------------------------
	// if it was read in successfully, then restore the game from the buffer & free the buffer

	if (errorCode == SR_OK)
	{
		errorCode = RestoreFromBuffer(saveBufferMem, bufferSize);

		// Note that the buffer has been freed inside RestoreFromBuffer,
		// in order to clear it from memory before loading in the new screen & runlist
	}
	else
		Free_mem( saveBufferMem );	// because RestoreFromBuffer would have freed it

	//------------------------------------------------------

	return(errorCode);												// game restored ok
}

//------------------------------------------------------------------------------------

uint32 RestoreData(uint16 slotNo, uint8 *buffer, uint32 bufferSize)
{
	char saveFileName[MAX_FILENAME_LEN];
	FILE *fp;
 	uint32	itemsRead;

  
	sprintf(saveFileName, "saves\\savegame.%.3d", slotNo);	// construct filename

	fp = fopen(saveFileName, "rb");					// attempt to open file for reading

	if (fp==NULL)
	{
		return(SR_ERR_FILEOPEN);					// error: couldn't open file
	}
	else
	{
//		itemsRead = fread(destAddress, size, count, fp);
		itemsRead = fread(buffer, 1, bufferSize, fp);	// read savegame into the buffer

		if (itemsRead == bufferSize)	// if we successfully read it all
		{
	 		fclose(fp);										// close savegame file
			return(SR_OK);				// file read ok
		}
		else	// didn't read the expected amount of data for some reason
		{
			if (ferror(fp))	// if it was a genuine read error, before reaching the end of the file
			{
	 			fclose(fp);					// close savegame file
   				return(SR_ERR_READFAIL);	// error: read failed
			}
			else	// we reached the end of the file before we filled the savegame buffer (ie. incompatible savegame file!)
 			{
	 			fclose(fp);						// close savegame file
				return(SR_ERR_INCOMPATIBLE);	// error: incompatible save-data - can't use!
			}
		}
	}
}

//------------------------------------------------------------------------------------
uint32	RestoreFromBuffer(mem *buffer, uint32 size)
{
	uint8	*varsRes;
	int32	pars[2];

	memcpy( &header, buffer->ad, sizeof(header) );		// get a copy of the header from the savegame buffer

  	//------------------------------------------------------
	// Calc checksum & check that aginst the value stored in the header (James05aug97)

	if (header.checksum != CalcChecksum((buffer->ad)+sizeof(header.checksum), size-sizeof(header.checksum)))
	{
		Free_mem( buffer );
		return(SR_ERR_INCOMPATIBLE);	// error: incompatible save-data - can't use!
	}
	//------------------------------------------------------
	// check savegame against length of current global variables resource
	// This would most probably be trapped by the checksum test anyway, but it doesn't do any harm to check this as well

	// Note that during development, earlier savegames will often be shorter than the current expected length

	if (header.varLength != res_man.Res_fetch_len(1))	// if header contradicts actual current size of global variables
	{
		Free_mem( buffer );
		return(SR_ERR_INCOMPATIBLE);	// error: incompatible save-data - can't use!
	}
	//----------------------------------
	// clean out system
	res_man.Kill_all_res(0);	// trash all resources from memory except player object & global variables
	LLogic.Reset_kill_list();	// clean out the system kill list (no more objects to kill)
	
	//----------------------------------
	// get player character data from savegame buffer

	// object hub is just after the standard header 
	memcpy (res_man.Res_open(CUR_PLAYER_ID) + sizeof(_standardHeader), &header.player_hub, sizeof(_object_hub));
	res_man.Res_close(CUR_PLAYER_ID);
	PutPlayerStructures();										// fill in the 4 essential player object structures from the header

	//----------------------------------
	// get variables resource from the savegame buffer	

	varsRes = res_man.Res_open(1);								// open variables resource
	memcpy( varsRes, buffer->ad + sizeof(header), header.varLength );// copy that to the buffer, following the header
 	res_man.Res_close(1);										// close variables resource

	Free_mem( buffer );		// free it now, rather than in RestoreGame, to unblock memory before new screen & runlist loaded
	pars[0] = header.screenId;
	pars[1] = 1;
	FN_init_background(pars);
	this_screen.new_palette=99;	// (JEL08oct97) so palette not restored immediately after control panel - we want to fade up instead!

	this_screen.feet_x = header.feet_x;	// these need setting after the defaults get set in FN_init_background
	this_screen.feet_y = header.feet_y;	// remember that these can change through the game, so need saving & restoring too
	LLogic.Express_change_session(header.runListId);			// start the new run list

	//----------------------------------------------------------------------------
	// (James01aug97)
	// Force in the new scroll position, so unsightly scroll-catch-up does not occur
	// when screen first draws after returning from restore panel

	this_screen.player_feet_x = header.mega.feet_x;	// set 'this_screen's record of player position
	this_screen.player_feet_y = header.mega.feet_y;	// - ready for Set_scrolling()

	if (this_screen.scroll_flag)	// if this screen is wide
		Set_scrolling();			// recompute the scroll offsets now, 

	//----------------------------------------------------------------------------
	// Any music required will be started after we've returned from Restore_control()
	// - see System_menu() in mouse.cpp!
	looping_music_id = header.music_id;
 	//------------------------------------------------------

	//--------------------------------------
	// Write to walkthrough file (zebug0.txt)
	#ifdef _DEBUG
	Zdebug(0,"*************************************");
	Zdebug(0,"RESTORED GAME \"%s\"", header.description);
	Zdebug(0,"*************************************");

	// Also write this to system debug file
 	Zdebug("*************************************");
	Zdebug("RESTORED GAME \"%s\"", header.description);
	Zdebug("*************************************");
 	#endif
	//--------------------------------------


	return(SR_OK);												// game restored ok

}

//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------
// GetSaveDescription - PC version...
//------------------------------------------------------------------------------------

uint32 GetSaveDescription(uint16 slotNo, uint8 *description)		// (James05feb97)
{
	char saveFileName[MAX_FILENAME_LEN];
	_savegameHeader header;
	FILE *fp;

	sprintf(saveFileName, "saves\\savegame.%.3d", slotNo);			// construct filename

	fp = fopen(saveFileName, "rb");							// attempt to open file for reading

	if (fp==NULL)
	{
		return(SR_ERR_FILEOPEN);							// error: couldn't open file
	}
	else
	{
//		fread(destAddress, size, count, fp);
		fread(&header, sizeof(header), 1, fp);				// read header
		fclose(fp);
		sprintf((char*)description, header.description);
		return(SR_OK);
	}
}

//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------
void GetPlayerStructures(void)		// James27feb97
{
	 // request the player object structures which need saving

	uint32	null_pc=7;	// script no. 7 - 'george_savedata_request' calls FN_pass_player_savedata
 	char	*raw_script_ad;
	_standardHeader	*head;


	head = (_standardHeader*) res_man.Res_open(CUR_PLAYER_ID);

	if	(head->fileType!=GAME_OBJECT)
		Con_fatal_error("incorrect CUR_PLAYER_ID=%d (%s line %u)",CUR_PLAYER_ID,__FILE__,__LINE__);

	raw_script_ad = (char *)head;	// (head+1) + sizeof(_object_hub);	//get to raw script data
	RunScript( raw_script_ad, raw_script_ad, &null_pc );
	res_man.Res_close(CUR_PLAYER_ID);
}
//------------------------------------------------------------------------------------
void PutPlayerStructures(void)		// James27feb97 (updated by James on 29july97)
{
	// fill out the player object structures from the savegame structures
	// also run the appropriate scripts to set up george's anim tables & walkdata, and nico's anim tables

	uint32	null_pc=8;	// script no. 8 - 'george_savedata_return' calls FN_get_player_savedata
 	char	*raw_script_ad;
	_standardHeader	*head;


	head = (_standardHeader*) res_man.Res_open(CUR_PLAYER_ID);

	if	(head->fileType!=GAME_OBJECT)
		Con_fatal_error("incorrect CUR_PLAYER_ID=%d (%s line %u)",CUR_PLAYER_ID,__FILE__,__LINE__);

	raw_script_ad = (char *)head;	// (head+1) + sizeof(_object_hub);	//get to raw script data
	
	null_pc=8;	// script no. 8 - 'george_savedata_return' calls FN_get_player_savedata
	RunScript( raw_script_ad, raw_script_ad, &null_pc );

	null_pc=14;	// script no. 14 - 'set_up_nico_anim_tables'
	RunScript( raw_script_ad, raw_script_ad, &null_pc );

	switch (header.mega.megaset_res)	// which megaset was the player at the time of saving?
	{
		case 36:		// GeoMega:
			null_pc=9;	// script no.9	- 'player_is_george'
			break;

		case 2003:		// GeoMegaB:
			null_pc=13;	// script no.13 - 'player_is_georgeB'
			break;

		case 1366:		// NicMegaA:
			null_pc=11;	// script no.11 - 'player_is_nicoA'
			break;

 		case 1437:		// NicMegaB:
			null_pc=12;	// script no.12 - 'player_is_nicoB'
			break;

 		case 1575:		// NicMegaC:
			null_pc=10;	// script no.10 - 'player_is_nicoC'
			break;
	}
	RunScript( raw_script_ad, raw_script_ad, &null_pc );

	res_man.Res_close(CUR_PLAYER_ID);
}
//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------
int32 FN_pass_player_savedata(int32 *params)	// James27feb97
{
	// copies the 4 essential player structures into the savegame header
	// - run script 7 of player object to request this

	// remember, we cannot simply read a compact any longer but instead must request it from the object itself

	// params:	0 pointer to object's logic structure
	//			1 pointer to object's graphic structure
	//			2 pointer to object's mega structure

	// copy from player object to savegame header
	memcpy( &header.logic,		(uint8*)params[0], sizeof(Object_logic)		);
	memcpy( &header.graphic,	(uint8*)params[1], sizeof(Object_graphic)	);
	memcpy( &header.mega,		(uint8*)params[2], sizeof(Object_mega)		);

	return(IR_CONT);	//makes no odds
}
//------------------------------------------------------------------------------------
int32 FN_get_player_savedata(int32 *params)	// James27feb97
{
	// reverse of FN_pass_player_savedata
	// - run script 8 of player object

	// params:	0 pointer to object's logic structure
	//			1 pointer to object's graphic structure
	//			2 pointer to object's mega structure

	Object_logic	*ob_logic		= (Object_logic*)	params[0];
	Object_graphic	*ob_graphic		= (Object_graphic*)	params[1];
	Object_mega		*ob_mega		= (Object_mega*)	params[2];

	int32	pars[3];


	// copy from savegame header to player object
	memcpy( (uint8*)ob_logic,	&header.logic,		sizeof(Object_logic)	);
	memcpy( (uint8*)ob_graphic,	&header.graphic,	sizeof(Object_graphic)	);
	memcpy( (uint8*)ob_mega,	&header.mega,		sizeof(Object_mega)		);


 	// any walk-data must be cleared - the player will be set to stand if he was walking when saved

	if (ob_mega->currently_walking)		// if the player was walking when game was saved
	{
		ob_mega->currently_walking = 0;	// clear the flag
		ob_mega->colliding = 0;			// reset this just in case

		pars[0] = (int32)ob_graphic;	// pointer to object's graphic structure
		pars[1] = (int32)ob_mega;		// pointer to object's mega structure
		pars[2] = ob_mega->current_dir;	// target direction
		FN_stand(pars);					// set player to stand

		ob_logic->looping = 0;			// reset looping flag (which would have been '1' during FN_walk)
	}


	return(IR_CONT);	//makes no odds
}
//------------------------------------------------------------------------------------
uint32 CalcChecksum(uint8 *buffer, uint32 size)		// (James05aug97)
{
	uint32 total=0;
	uint32 pos;

	for (pos=0; pos<size; pos++)
		total += buffer[pos];

	return(total);
}
