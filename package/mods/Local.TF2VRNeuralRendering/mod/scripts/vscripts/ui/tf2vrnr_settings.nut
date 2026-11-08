global function TF2VRNR_RegisterMenu
global function TF2VRNR_MenuLinks

struct
{
	var menu
	array<string> fields = [ "nr", "dlaa", "fg", "model", "resolution", "strength", "tone", "detail" ]
	array<string> labels = [ "Neural Rendering", "DLAA", "Frame generation (experimental)", "Neural model", "Model resolution", "Strength", "Tone", "Detail" ]
	array<string> descriptions = [
		"Neural lighting and detail, applied to both eyes.\n\nDLAA and frame generation have their own switches. Choose Apply to save your changes.",
		"Native-resolution DLSS anti-aliasing, using preset K.\n\nWorks independently of Neural Rendering. This controls anti-aliasing; it does not lower the game's render resolution.",
		"Experimental stereo frame interpolation.\n\nIt reduced smoothness in testing on this setup. Leave Off for normal play; turn it on only to compare.\n\nRequires valid stereo depth and motion data.",
		"Choose neural model A or B.\n\nThese are the two models tested with this addon. This changes Neural Rendering, not the DLAA preset.",
		"Resolution used by the neural model, as a percentage of each eye dimension.\n\n100% processes the full-size image. Lower values reduce neural processing work.\n\nThis leaves the game's VR render resolution unchanged.",
		"Overall Neural Rendering strength, from 0% to 100%.\n\nThis affects the image while Neural Rendering is on. Use its Off switch to stop neural processing entirely.",
		"Strength of the neural lighting and colour response, from 0% to 100%.\n\nRequires Neural Rendering to be on.",
		"Strength of neural material detail, from 0% to 100%.\n\nRequires Neural Rendering to be on."
	]
	array<var> rows
	array<var> buttons
	bool refreshing = false
	bool busy = true
	int generation = 0
} file

void function TF2VRNR_RegisterMenu()
{
	AddMenu( "TF2VRNRSettings", $"resource/ui/menus/tf2vrnr_settings.menu", TF2VRNR_Init )
	ModSettings_AddModTitle( "DLSS 5 / VR" )
	ModSettings_AddModCategory( "Neural Rendering and anti-aliasing" )
	ModSettings_AddButton( "Open DLSS 5 / VR settings", TF2VRNR_Open )
}

void function TF2VRNR_MenuLinks()
{
	// Add links without replacing the VR mod's menus or scripts.
	if ( "TF2VRSettings" in uiGlobal.menus )
		AddMenuFooterOption( GetMenu( "TF2VRSettings" ), BUTTON_X, PrependControllerPrompts( BUTTON_X, "DLSS 5" ), "DLSS 5", TF2VRNR_Open )
	if ( "InGameSPMenu" in uiGlobal.menus )
		AddMenuFooterOption( GetMenu( "InGameSPMenu" ), BUTTON_Y, PrependControllerPrompts( BUTTON_Y, "DLSS 5" ), "DLSS 5", TF2VRNR_Open )
	printt( "[TF2VRNR] DLSS 5 / VR menu registered" )
}

void function TF2VRNR_Open()
{
	AdvanceMenu( GetMenu( "TF2VRNRSettings" ) )
}

void function TF2VRNR_Init()
{
	file.menu = GetMenu( "TF2VRNRSettings" )
	for ( int i = 0; i < file.fields.len(); i++ )
	{
		var row = Hud_GetChild( file.menu, "Row_" + file.fields[i] )
		var button = row
		if ( i >= 5 )
			button = Hud_GetChild( row, "BtnDropButton" )
		else if ( i < 3 )
		{
			Hud_DialogList_AddListItem( row, "#SETTING_OFF", "0" )
			Hud_DialogList_AddListItem( row, "#SETTING_ON", "1" )
		}
		else if ( i == 3 )
		{
			Hud_DialogList_AddListItem( row, "Model A", "0" )
			Hud_DialogList_AddListItem( row, "Model B", "1" )
		}
		else
		{
			for ( int percent = 25; percent <= 100; percent += 25 )
				Hud_DialogList_AddListItem( row, string( percent ) + "%", string( percent ) )
		}
		SetButtonRuiText( button, file.labels[i] )
		AddButtonEventHandler( row, UIE_CHANGE, TF2VRNR_Changed )
		AddButtonEventHandler( button, UIE_GET_FOCUS, TF2VRNR_Focused )
		file.rows.append( row )
		file.buttons.append( button )
	}
	var apply = Hud_GetChild( file.menu, "BtnApply" )
	var revert = Hud_GetChild( file.menu, "BtnRevert" )
	var off = Hud_GetChild( file.menu, "BtnAllOff" )
	SetButtonRuiText( apply, "APPLY CHANGES" )
	SetButtonRuiText( revert, "REVERT / RELOAD SAVED SETTINGS" )
	SetButtonRuiText( off, "ALL EFFECTS OFF" )
	AddButtonEventHandler( apply, UIE_CLICK, TF2VRNR_ApplyButton )
	AddButtonEventHandler( revert, UIE_CLICK, TF2VRNR_RevertButton )
	AddButtonEventHandler( off, UIE_CLICK, TF2VRNR_AllOffButton )
	array<var> nav = clone file.buttons
	nav.append( apply )
	nav.append( revert )
	nav.append( off )
	SetNavUpDown( nav )
	AddMenuEventHandler( file.menu, eUIEvent.MENU_OPEN, TF2VRNR_Opened )
	AddMenuEventHandler( file.menu, eUIEvent.MENU_CLOSE, TF2VRNR_Closed )
	AddMenuFooterOption( file.menu, BUTTON_B, "#B_BUTTON_BACK", "#BACK" )
	AddMenuFooterOption( file.menu, BUTTON_X, "#X_BUTTON_APPLY", "#APPLY", TF2VRNR_Apply )
}

void function TF2VRNR_Status( string text )
{
	Hud_SetText( Hud_GetChild( file.menu, "Status" ), text )
}

void function TF2VRNR_SetBusy( bool busy )
{
	file.busy = busy
	foreach ( var row in file.rows )
		Hud_SetEnabled( row, !busy )
	foreach ( var button in file.buttons )
		Hud_SetEnabled( button, !busy )
	Hud_SetEnabled( Hud_GetChild( file.menu, "BtnApply" ), !busy )
	Hud_SetEnabled( Hud_GetChild( file.menu, "BtnAllOff" ), !busy )
	Hud_SetEnabled( Hud_GetChild( file.menu, "BtnRevert" ), !busy )
}

void function TF2VRNR_Refresh()
{
	file.refreshing = true
	for ( int i = 0; i < file.fields.len(); i++ )
	{
		int value = GetConVarInt( "tf2vrnr_saved_" + file.fields[i] )
		if ( i < 5 )
			Hud_SetDialogListSelectionValue( file.rows[i], string( value ) )
		else
		{
			Hud_SliderControl_SetMin( file.rows[i], float( value ) )
			Hud_SliderControl_SetMax( file.rows[i], float( value ) )
			Hud_SliderControl_SetMin( file.rows[i], 0.0 )
			Hud_SliderControl_SetMax( file.rows[i], 100.0 )
			SetButtonRuiText( file.buttons[i], file.labels[i] + "   " + string( value ) + "%" )
		}
		SetConVarInt( "tf2vrnr_edit_" + file.fields[i], value )
	}
	SetConVarInt( "tf2vrnr_base", GetConVarInt( "tf2vrnr_revision" ) )
	file.refreshing = false
}

void function TF2VRNR_Opened()
{
	file.generation++
	UI_SetPresentationType( ePresentationType.NO_MODELS )
	TF2VRNR_Request( 1 )
}

void function TF2VRNR_Closed()
{
	file.generation++
	// Closing discards unapplied edits. Persistence belongs to the INI bridge.
}

void function TF2VRNR_Request( int command )
{
	TF2VRNR_SetBusy( true )
	TF2VRNR_Status( command == 2 ? "Saving settings..." : "Loading saved settings..." )
	int serial = GetConVarInt( "tf2vrnr_request" ) + 1
	if ( serial > 1000000 ) serial = 1
	SetConVarInt( "tf2vrnr_command", command )
	SetConVarInt( "tf2vrnr_request", serial )
	thread TF2VRNR_WaitReply( serial, file.generation )
}

void function TF2VRNR_WaitReply( int serial, int generation )
{
	float deadline = Time() + 10.0
	while ( GetConVarInt( "tf2vrnr_ack" ) != serial || GetConVarInt( "tf2vrnr_ready" ) == 0 )
	{
		if ( generation != file.generation ) return
		if ( Time() > deadline )
		{
			TF2VRNR_Status( "The addon is not responding. Restart the game after installing the menu update, then choose Reload." )
			Hud_SetEnabled( Hud_GetChild( file.menu, "BtnRevert" ), true )
			return
		}
		WaitFrame()
	}
	if ( generation != file.generation ) return
	int status = GetConVarInt( "tf2vrnr_status" )
	TF2VRNR_Refresh()
	TF2VRNR_SetBusy( false )
	string text = "Saved settings loaded. Changes take effect when you choose Apply."
	if ( status == 2 ) text = "Saved. The renderer will update shortly. A brief pause while effects reload is normal."
	else if ( status == -1 ) text = "Could not read or save the addon settings. Check the addon folder and try Reload."
	else if ( status == -2 ) text = "A required NVIDIA runtime is missing. No changes saved."
	else if ( status == -3 ) text = "A value was outside its supported range. No changes saved."
	else if ( status == -4 ) text = "The addon layer is disabled. Enable it and restart the game before enabling effects."
	else if ( status == -5 ) text = "Settings changed outside this menu. Loaded the newer settings; make your edits again."
	else if ( status == -6 ) text = "Settings saved, but the live update request failed. Apply again, or restart the game."
	TF2VRNR_Status( text )
}

void function TF2VRNR_Changed( var row )
{
	if ( file.refreshing || file.busy ) return
	for ( int i = 0; i < file.rows.len(); i++ )
	{
		if ( file.rows[i] != row ) continue
		int value = 0
		if ( i < 5 ) value = int( Hud_GetDialogListSelectionValue( row ) )
		else
		{
			value = int( Hud_SliderControl_GetCurrentValue( row ) + 0.5 )
			SetButtonRuiText( file.buttons[i], file.labels[i] + "   " + string( value ) + "%" )
		}
		SetConVarInt( "tf2vrnr_edit_" + file.fields[i], value )
		TF2VRNR_Status( "Changes pending. Choose Apply to save, or Revert to discard." )
		return
	}
}

void function TF2VRNR_Focused( var button )
{
	for ( int i = 0; i < file.buttons.len(); i++ )
		if ( file.buttons[i] == button )
			Hud_SetText( Hud_GetChild( file.menu, "Description" ), file.descriptions[i] )
}

void function TF2VRNR_Apply()
{
	if ( file.busy ) return
	TF2VRNR_Request( 2 )
}
void function TF2VRNR_ApplyButton( var button ) { TF2VRNR_Apply() }
void function TF2VRNR_RevertButton( var button ) { TF2VRNR_Request( 1 ) }
void function TF2VRNR_AllOffButton( var button )
{
	if ( file.busy ) return
	SetConVarInt( "tf2vrnr_edit_nr", 0 )
	SetConVarInt( "tf2vrnr_edit_dlaa", 0 )
	SetConVarInt( "tf2vrnr_edit_fg", 0 )
	TF2VRNR_Request( 2 )
}
