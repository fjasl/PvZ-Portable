/*
 * Copyright (C) 2026 Zhou Qiankang <wszqkzqk@qq.com>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable.
 *
 * PvZ-Portable is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PvZ-Portable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with PvZ-Portable. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Gamepad support for the mouse-driven PvZ UI.
 *
 * PvZ's UI model is "move a pointer, click widgets", so the cleanest way to add
 * gamepad control is to synthesize that pointer instead of teaching every
 * widget about gamepads:
 *
 *   - Left stick / D-pad move a virtual pointer over the 800x600 screen space.
 *   - A = left click, B = right click (cancel), X = shovel, Start = ESC.
 *   - LB / RB cycle through the available seed packets by clicking them.
 *   - While a seed (or the shovel) is held, the pointer snaps to the centre of
 *     the lawn grid cell under the raw pointer position. This gives
 *     console-style "grid cursor" planting without changing any game logic:
 *     Board::PixelToGridX/Y does the cell lookup and GridToPixelX/Y gives us
 *     the centre, so plant legality, cooldowns and animations stay untouched.
 *
 * Real touch input still works: whenever the framework's mouse position is
 * changed by something other than us, we adopt it as the new pointer origin so
 * the two input methods never fight.
 */

#include <SDL.h>

#include <algorithm>
#include <cmath>

#include "../Board.h"
#include "../CursorObject.h"
#include "../SeedPacket.h"
#include "../../LawnApp.h"
#include "misc/KeyCodes.h"
#include "widget/WidgetManager.h"

using namespace Sexy;

namespace
{
// Virtual screen space used by the game (see GLInterface: PvZ is 800x600).
constexpr int SCREEN_W = 800;
constexpr int SCREEN_H = 600;

constexpr float STICK_DEADZONE = 0.22f;
constexpr float POINTER_SPEED_X = 900.0f; // virtual pixels / second
constexpr float POINTER_SPEED_Y = 700.0f;

enum PadButton
{
	PAD_A = 0,
	PAD_B,
	PAD_X,
	PAD_Y,
	PAD_LB,
	PAD_RB,
	PAD_START,
	PAD_BACK,
	PAD_UP,
	PAD_DOWN,
	PAD_LEFT,
	PAD_RIGHT,
	PAD_BUTTON_COUNT
};

SDL_GameController* gPad = nullptr;
// Fallback for pads that are not in SDL's controller database (common on
// Chinese Android handhelds): read the raw joystick instead.
SDL_Joystick* gStick = nullptr;
bool gSDLSubsystemInited = false;

float gPointerX = SCREEN_W / 2.0f;
float gPointerY = SCREEN_H / 2.0f;
int gSentX = -1;
int gSentY = -1;
bool gPointerReady = false;
bool gWasGridMode = false;
Uint32 gLastTick = 0;
uint32_t gButtonsPrev = 0;
int gPacketCursor = -1;

bool PadIsOpen()
{
	if (gPad != nullptr)
		return SDL_GameControllerGetAttached(gPad) == SDL_TRUE;

	if (gStick != nullptr)
		return SDL_JoystickGetAttached(gStick) == SDL_TRUE;

	return false;
}

bool EnsurePadOpen()
{
	if (!gSDLSubsystemInited)
	{
		if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0)
			SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
		gSDLSubsystemInited = true;
	}

	if (PadIsOpen())
		return true;

	if (gPad != nullptr)
	{
		SDL_GameControllerClose(gPad);
		gPad = nullptr;
	}

	if (gStick != nullptr)
	{
		SDL_JoystickClose(gStick);
		gStick = nullptr;
	}

	// Poll instead of relying on SDL_CONTROLLERDEVICEADDED: the main event pump
	// consumes and drops unknown event types, so hot-plug is detected here.
	int aNumJoysticks = SDL_NumJoysticks();
	for (int i = 0; i < aNumJoysticks; i++)
	{
		if (!SDL_IsGameController(i))
			continue;

		gPad = SDL_GameControllerOpen(i);
		if (gPad != nullptr)
		{
			SDL_Log("PvZ: controller opened: %s", SDL_GameControllerName(gPad));
			return true;
		}
	}

	for (int i = 0; i < aNumJoysticks; i++)
	{
		SDL_Joystick* aStick = SDL_JoystickOpen(i);
		if (aStick == nullptr)
			continue;

		// Android exposes sensors (e.g. "Android Accelerometer") as joysticks,
		// and some pads report no buttons at all. Neither can drive a pointer.
		const char* aName = SDL_JoystickName(aStick);
		bool aUsable = SDL_JoystickNumAxes(aStick) >= 2 && SDL_JoystickNumButtons(aStick) >= 4 &&
			(aName == nullptr || SDL_strcasestr(aName, "accelerometer") == nullptr);

		if (aUsable)
		{
			gStick = aStick;
			SDL_Log("PvZ: raw joystick opened (no SDL mapping): %s", aName);
			return true;
		}

		SDL_JoystickClose(aStick);
	}

	return false;
}

float RawAxis(int theIndex)
{
	if (gStick == nullptr)
		return 0.0f;

	return SDL_JoystickGetAxis(gStick, theIndex) / 32767.0f;
}

float AxisLeftX()
{
	if (gPad != nullptr)
		return SDL_GameControllerGetAxis(gPad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;

	return RawAxis(0);
}

float AxisLeftY()
{
	if (gPad != nullptr)
		return SDL_GameControllerGetAxis(gPad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;

	// HID pads usually report "up" as negative Y, same as SDL controllers.
	return RawAxis(1);
}

float ApplyDeadzone(float theValue)
{
	float anAbs = std::fabs(theValue);
	if (anAbs <= STICK_DEADZONE)
		return 0.0f;

	// Rescale to 0..1 and square it: fine control near the centre, full speed
	// at the edge, which is what makes placing a seed feel precise.
	float aNormalized = (anAbs - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
	return std::copysign(aNormalized * aNormalized, theValue);
}

uint32_t ReadButtons()
{
	uint32_t aBits = 0;
	auto set = [&aBits](PadButton theButton, bool theDown)
	{
		if (theDown)
			aBits |= (1u << theButton);
	};

	if (gPad != nullptr)
	{
		set(PAD_A, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_A) != 0);
		set(PAD_B, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_B) != 0);
		set(PAD_X, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_X) != 0);
		set(PAD_Y, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_Y) != 0);
		set(PAD_LB, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0);
		set(PAD_RB, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0);
		set(PAD_START, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_START) != 0);
		set(PAD_BACK, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_BACK) != 0);
		set(PAD_UP, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0);
		set(PAD_DOWN, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0);
		set(PAD_LEFT, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0);
		set(PAD_RIGHT, SDL_GameControllerGetButton(gPad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0);
	}
	else if (gStick != nullptr)
	{
		// Heuristic layout for unrecognised HID pads: the Android/Xbox button
		// order (A, B, X, Y, LB, RB, Back, Start) is by far the most common.
		static constexpr int RAW_BUTTON_MAP[] = {0, 1, 2, 3, 4, 5, 7, 6};
		for (int aButton = 0; aButton < 8; aButton++)
		{
			set((PadButton)aButton, SDL_JoystickGetButton(gStick, RAW_BUTTON_MAP[aButton]) != 0);
		}

		int aHat = SDL_JoystickGetHat(gStick, 0);
		set(PAD_UP, (aHat & SDL_HAT_UP) != 0);
		set(PAD_DOWN, (aHat & SDL_HAT_DOWN) != 0);
		set(PAD_LEFT, (aHat & SDL_HAT_LEFT) != 0);
		set(PAD_RIGHT, (aHat & SDL_HAT_RIGHT) != 0);
	}

	return aBits;
}

void SendMouseMove(LawnApp* theApp, int theX, int theY)
{
	if (!theApp->mMouseIn)
		theApp->mMouseIn = true;

	theApp->mWidgetManager->MouseMove(theX, theY);
	theApp->mLastUserInputTick = theApp->mLastTimerTime;
	gSentX = theX;
	gSentY = theY;
}

void SendClick(LawnApp* theApp, int theX, int theY, int theButton)
{
	if (!theApp->mMouseIn)
		theApp->mMouseIn = true;

	// MouseMove first so the hit test inside MouseDown sees the same position
	// a real pointer would have reported.
	theApp->mWidgetManager->MouseMove(theX, theY);
	theApp->mWidgetManager->MouseDown(theX, theY, theButton);
	theApp->mWidgetManager->MouseUp(theX, theY, theButton);

	theApp->mLastUserInputTick = theApp->mLastTimerTime;
	gSentX = theX;
	gSentY = theY;
}

void SendEscape(LawnApp* theApp)
{
	theApp->mWidgetManager->KeyDown(KEYCODE_ESCAPE);
	theApp->mWidgetManager->KeyUp(KEYCODE_ESCAPE);
	theApp->mLastUserInputTick = theApp->mLastTimerTime;
}

bool CursorTypeUsesGrid(CursorType theCursorType)
{
	switch (theCursorType)
	{
	case CursorType::CURSOR_TYPE_PLANT_FROM_BANK:
	case CursorType::CURSOR_TYPE_PLANT_FROM_USABLE_COIN:
	case CursorType::CURSOR_TYPE_PLANT_FROM_GLOVE:
	case CursorType::CURSOR_TYPE_PLANT_FROM_DUPLICATOR:
	case CursorType::CURSOR_TYPE_PLANT_FROM_WHEEL_BARROW:
	case CursorType::CURSOR_TYPE_SHOVEL:
		return true;
	default:
		return false;
	}
}

// Centre of a lawn cell, derived from the game's own grid mapping so that roof
// slopes, pool rows and conveyor-belt stages all keep working.
void GetCellCentre(Board* theBoard, int theGridX, int theGridY, int& theX, int& theY)
{
	theX = theBoard->GridToPixelX(theGridX, theGridY) + 40;

	int aTop = theBoard->GridToPixelY(theGridX, theGridY);
	int aCellHeight = 100;
	if (theGridY + 1 < MAX_GRID_SIZE_Y)
		aCellHeight = theBoard->GridToPixelY(theGridX, theGridY + 1) - aTop;
	else if (theGridY > 0)
		aCellHeight = aTop - theBoard->GridToPixelY(theGridX, theGridY - 1);

	theY = aTop + aCellHeight / 2;
}

// Index of the next (direction = 1) or previous (direction = -1) seed packet
// that the player could actually pick up, or -1 if there is none.
int FindNextPacket(Board* theBoard, int theFrom, int theDirection)
{
	SeedBank* aSeedBank = theBoard->mSeedBank.get();
	if (aSeedBank == nullptr)
		return -1;

	int aNumPackets = std::min<int>(aSeedBank->mNumPackets, SEEDBANK_MAX);
	if (aNumPackets <= 0)
		return -1;

	for (int aStep = 1; aStep <= aNumPackets; aStep++)
	{
		int anIndex = ((theFrom < 0 ? (theDirection > 0 ? -1 : 0) : theFrom) + aStep * theDirection) % aNumPackets;
		if (anIndex < 0)
			anIndex += aNumPackets;

		SeedPacket& aPacket = aSeedBank->mSeedPackets[anIndex];
		if (aPacket.mPacketType != SeedType::SEED_NONE && aPacket.mSlotMachineCountDown <= 0)
			return anIndex;
	}

	return -1;
}

void ClickSeedPacket(LawnApp* theApp, Board* theBoard, int theIndex)
{
	SeedBank* aSeedBank = theBoard->mSeedBank.get();
	if (aSeedBank == nullptr || theIndex < 0 || theIndex >= SEEDBANK_MAX)
		return;

	SeedPacket& aPacket = aSeedBank->mSeedPackets[theIndex];
	int aPacketX = aSeedBank->mX + aPacket.mX + aPacket.mOffsetX + aPacket.mWidth / 2;
	int aPacketY = aSeedBank->mY + aPacket.mY + aPacket.mHeight / 2;

	SendClick(theApp, aPacketX, aPacketY, 1);
	gPacketCursor = theIndex;
}

void ClickShovel(LawnApp* theApp, Board* theBoard)
{
	Rect aRect = theBoard->GetShovelButtonRect();
	SendClick(theApp, aRect.mX + aRect.mWidth / 2, aRect.mY + aRect.mHeight / 2, 1);
}
} // namespace

void LawnApp::UpdateGamepadInput()
{
	if (!EnsurePadOpen())
		return;

	uint32_t aNow = SDL_GetTicks();
	float aDeltaTime = (aNow - gLastTick) / 1000.0f;
	gLastTick = aNow;
	if (aDeltaTime <= 0.0f || aDeltaTime > 0.1f)
		aDeltaTime = 1.0f / 60.0f;

	if (!gPointerReady)
	{
		gPointerX = (float)mWidgetManager->mLastMouseX;
		gPointerY = (float)mWidgetManager->mLastMouseY;
		gSentX = mWidgetManager->mLastMouseX;
		gSentY = mWidgetManager->mLastMouseY;
		gPointerReady = true;
	}

	// Something else (touch, or the desktop mouse) moved the pointer: adopt it
	// and let this frame be a no-op so the two input paths never fight.
	if (mWidgetManager->mLastMouseX != gSentX || mWidgetManager->mLastMouseY != gSentY)
	{
		gPointerX = (float)mWidgetManager->mLastMouseX;
		gPointerY = (float)mWidgetManager->mLastMouseY;
		gSentX = mWidgetManager->mLastMouseX;
		gSentY = mWidgetManager->mLastMouseY;
		gButtonsPrev = ReadButtons();
		return;
	}

	float aAxisX = ApplyDeadzone(AxisLeftX());
	float aAxisY = ApplyDeadzone(AxisLeftY());

	uint32_t aButtons = ReadButtons();
	uint32_t aPressed = aButtons & ~gButtonsPrev;
	gButtonsPrev = aButtons;

	// The D-pad is digital: full speed, and it lets the player nudge the
	// pointer one cell at a time while a seed is held.
	if (aButtons & (1u << PAD_LEFT))
		aAxisX = -1.0f;
	else if (aButtons & (1u << PAD_RIGHT))
		aAxisX = 1.0f;

	if (aButtons & (1u << PAD_UP))
		aAxisY = -1.0f;
	else if (aButtons & (1u << PAD_DOWN))
		aAxisY = 1.0f;

	if (aAxisX != 0.0f || aAxisY != 0.0f)
	{
		gPointerX += aAxisX * POINTER_SPEED_X * aDeltaTime;
		gPointerY += aAxisY * POINTER_SPEED_Y * aDeltaTime;
	}

	gPointerX = std::clamp(gPointerX, 0.0f, (float)(SCREEN_W - 1));
	gPointerY = std::clamp(gPointerY, 0.0f, (float)(SCREEN_H - 1));

	Board* aBoard = (mGameScene == GameScenes::SCENE_PLAYING) ? mBoard : nullptr;

	bool aGridMode = false;
	if (aBoard != nullptr && aBoard->mCursorObject != nullptr &&
		aBoard->mSeedBank != nullptr && CursorTypeUsesGrid(aBoard->mCursorObject->mCursorType))
	{
		aGridMode = true;
	}

	int aTargetX = (int)std::lround(gPointerX);
	int aTargetY = (int)std::lround(gPointerY);

	if (aGridMode)
	{
		int aGridX = aBoard->PixelToGridX(aTargetX, aTargetY);
		int aGridY = aBoard->PixelToGridY(aTargetX, aTargetY);

		// Outside the lawn (e.g. the seed bank strip, or the shovel) keep free
		// movement so packets stay clickable.
		if (aGridX >= 0 && aGridY >= 0)
		{
			int aCellX = 0;
			int aCellY = 0;
			GetCellCentre(aBoard, aGridX, aGridY, aCellX, aCellY);
			aTargetX = aCellX;
			aTargetY = aCellY;
		}
	}

	if (gWasGridMode && !aGridMode)
	{
		// Leaving grid mode: keep the pointer where the player last saw it
		// instead of letting it jump back to the raw position.
		gPointerX = (float)gSentX;
		gPointerY = (float)gSentY;
		aTargetX = gSentX;
		aTargetY = gSentY;
	}
	gWasGridMode = aGridMode;

	if (aTargetX != gSentX || aTargetY != gSentY)
		SendMouseMove(this, aTargetX, aTargetY);

	if (aPressed & (1u << PAD_A))
		SendClick(this, aTargetX, aTargetY, 1);

	if (aPressed & (1u << PAD_B))
		SendClick(this, aTargetX, aTargetY, -1); // right click = cancel selection

	if (aPressed & (1u << PAD_X))
	{
		if (aBoard != nullptr && aBoard->mShowShovel)
			ClickShovel(this, aBoard);
	}

	if (aPressed & (1u << PAD_START))
		SendEscape(this);

	if (aPressed & (1u << PAD_BACK))
		SendEscape(this);
	else if (aPressed & (1u << PAD_LB))
	{
		if (aBoard != nullptr)
		{
			int anIndex = FindNextPacket(aBoard, gPacketCursor, -1);
			if (anIndex >= 0)
				ClickSeedPacket(this, aBoard, anIndex);
		}
	}
	else if (aPressed & (1u << PAD_RB))
	{
		if (aBoard != nullptr)
		{
			int anIndex = FindNextPacket(aBoard, gPacketCursor, 1);
			if (anIndex >= 0)
				ClickSeedPacket(this, aBoard, anIndex);
		}
	}
}
