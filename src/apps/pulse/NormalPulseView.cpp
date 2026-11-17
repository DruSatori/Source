//*****************************************************************************
//
//	File:		NormalPulseView.cpp
//
//	Written by:	Daniel Switkin
//
//	Copyright 1999, Be Incorporated
//
//*****************************************************************************


#include "NormalPulseView.h"
#include "Common.h"
#include "Pictures"

#include <Catalog.h>
#include <Bitmap.h>
#include <Dragger.h>
#include <IconUtils.h>
#include <Window.h>

#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <cpu_type.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "NormalPulseView"


NormalPulseView::NormalPulseView(BRect rect)
	: PulseView(rect, "NormalPulseView"),
	fBrandLogo(NULL)
{
	SetResizingMode(B_NOT_RESIZABLE);
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	SetLowUIColor(ViewUIColor());

	mode1->SetLabel(B_TRANSLATE("Mini mode"));
	mode1->SetMessage(new BMessage(PV_MINI_MODE));
	mode2->SetLabel(B_TRANSLATE("Deskbar mode"));
	mode2->SetMessage(new BMessage(PV_DESKBAR_MODE));

	DetermineVendorAndProcessor();
	BFont font(be_plain_font);
	font.SetSize(8);
	SetFont(&font);

	float width = std::max(StringWidth(fProcessor), 48.0f);
	fChipRect = BRect(10, (rect.Height() - width - 15) / 2, 25 + width,
		(rect.Height() + width + 15) / 2);
	float progressLeft = fChipRect.right + 29;
	float cpuLeft = fChipRect.right + 5;

	// Allocate progress bars and button pointers
	system_info systemInfo;
	get_system_info(&systemInfo);
	fCpuCount = systemInfo.cpu_count;
	fProgressBars = new ProgressBar *[fCpuCount];
	fCpuButtons = new CPUButton *[fCpuCount];

	// Set up the CPU activity bars and buttons
	for (int x = 0; x < fCpuCount; x++) {
		BRect r(progressLeft, PROGRESS_MTOP + ITEM_OFFSET * x,
			progressLeft + ProgressBar::PROGRESS_WIDTH,
			PROGRESS_MTOP + ITEM_OFFSET * x + ProgressBar::PROGRESS_HEIGHT);
		char* str2 = (char *)B_TRANSLATE("CPU progress bar");
		fProgressBars[x] = new ProgressBar(r, str2);
		AddChild(fProgressBars[x]);

		r.Set(cpuLeft, CPUBUTTON_MTOP + ITEM_OFFSET * x,
			cpuLeft + CPUBUTTON_WIDTH + 7,
			CPUBUTTON_MTOP + ITEM_OFFSET * x + CPUBUTTON_HEIGHT + 7);
		char temp[4];
		snprintf(temp, sizeof(temp), "%hhd", int8(x + 1));
		fCpuButtons[x] = new CPUButton(r, B_TRANSLATE_SYSTEM_NAME("Pulse"), temp, NULL);
		AddChild(fCpuButtons[x]);
	}

	if (fCpuCount == 1) {
		fProgressBars[0]->MoveBy(-3, 12);
		fCpuButtons[0]->Hide();
	}

	ResizeTo(progressLeft + ProgressBar::PROGRESS_WIDTH + 10, rect.Height());
}


NormalPulseView::~NormalPulseView()
{
	delete fBrandLogo;
	delete[] fCpuButtons;
	delete[] fProgressBars;
}


void
NormalPulseView::DetermineVendorAndProcessor()
{
	system_info sys_info;
	get_system_info(&sys_info);

	// Initialize logo

	const unsigned char* logo = NULL;
	size_t logoSize = 0;
	uint32 topologyNodeCount = 0;
	cpu_topology_node_info* topology = NULL;
	enum cpu_platform platform = B_CPU_UNKNOWN;
	uint32 isaLevel = 0;

	get_cpu_topology_info(NULL, &topologyNodeCount);
	if (topologyNodeCount != 0)
		topology = new cpu_topology_node_info[topologyNodeCount];
	get_cpu_topology_info(topology, &topologyNodeCount);

	for (uint32 i = 0; i < topologyNodeCount; i++) {
		if (topology[i].type == B_TOPOLOGY_ROOT)
			platform = topology[i].data.root.platform;

		// Use less specific platform logo only if no vendor specific one is
		// available
		if (logo == NULL && topology[i].type == B_TOPOLOGY_ROOT) {
			switch (topology[i].data.root.platform) {
				case B_CPU_RISC_V:
					logo = kRiscVLogo;
					logoSize = sizeof(kRiscVLogo);
					break;
				default:
					break;
			}
		}

		if (topology[i].type == B_TOPOLOGY_PACKAGE) {
			isaLevel = topology[i].data.package.isa_level;
			switch (topology[i].data.package.vendor) {
				case B_CPU_VENDOR_AMD:
					logo = kAmdLogo;
					logoSize = sizeof(kAmdLogo);
					break;

				case B_CPU_VENDOR_CYRIX:
					logo = kCyrixLogo;
					logoSize = sizeof(kCyrixLogo);
					break;

				case B_CPU_VENDOR_INTEL:
					logo = kIntelLogo;
					logoSize = sizeof(kIntelLogo);
					break;

				case B_CPU_VENDOR_MOTOROLA:
					logo = kPowerPCLogo;
					logoSize = sizeof(kPowerPCLogo);
					break;

				case B_CPU_VENDOR_VIA:
					logo = kViaLogo;
					logoSize = sizeof(kViaLogo);
					break;

				default:
					break;
			}

			break;
		}
	}

	delete[] topology;

	const char* isa = get_cpu_isa_string(platform, isaLevel);
	strlcpy(fIsa, isa != NULL ? isa : "", sizeof(fIsa));

	if (logo != NULL) {
		fBrandLogo = new BBitmap(BRect(0, 0, 47, 47), B_RGBA32);
		if (BIconUtils::GetVectorIcon(logo, logoSize, fBrandLogo) != B_OK) {
			delete fBrandLogo;
			fBrandLogo = NULL;
		}
	} else
		fBrandLogo = NULL;

	get_cpu_type(fVendor, sizeof(fVendor), fProcessor, sizeof(fProcessor));
}


void
NormalPulseView::DrawChip(BRect r)
{
	SetDrawingMode(B_OP_COPY);
	BRect innerRect = r.InsetByCopy(7, 7);
	SetHighColor(0x20, 0x20, 0x20);
	FillRect(innerRect);

	innerRect.InsetBy(-1, -1);
	SetHighColor(0x40, 0x40, 0x40);
	SetLowColor(0x48, 0x48, 0x48);
	StrokeRect(innerRect, B_MIXED_COLORS);

	innerRect.InsetBy(-1, -1);
	SetHighColor(0x78, 0x78, 0x78);
	StrokeRect(innerRect);

	innerRect.InsetBy(-1, -1);
	SetHighColor(0x08, 0x08, 0x08);
	SetLowColor(0x20, 0x20, 0x20);
	StrokeLine(BPoint(innerRect.left, innerRect.top + 1),
		BPoint(innerRect.left, innerRect.bottom - 1), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.right, innerRect.top + 1),
		BPoint(innerRect.right, innerRect.bottom - 1), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.left + 1, innerRect.top),
		BPoint(innerRect.right - 1, innerRect.top), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.left + 1, innerRect.bottom),
		BPoint(innerRect.right - 1, innerRect.bottom), B_MIXED_COLORS);

	innerRect.InsetBy(-1, -1);
	SetLowColor(0xff, 0xff, 0xff);
	SetHighColor(0x20, 0x20, 0x20);
	StrokeLine(BPoint(innerRect.left, innerRect.top + 6),
		BPoint(innerRect.left, innerRect.bottom - 6), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.right, innerRect.top + 6),
		BPoint(innerRect.right, innerRect.bottom - 6), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.left + 6, innerRect.top),
		BPoint(innerRect.right - 6, innerRect.top), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.left + 6, innerRect.bottom),
		BPoint(innerRect.right - 6, innerRect.bottom), B_MIXED_COLORS);

	innerRect.InsetBy(-1, -1);
	SetHighColor(0xa8, 0xa8, 0xa8);
	SetLowColor(0x20, 0x20, 0x20);
	StrokeLine(BPoint(innerRect.left, innerRect.top + 7),
		BPoint(innerRect.left, innerRect.bottom - 7), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.right, innerRect.top + 7),
		BPoint(innerRect.right, innerRect.bottom - 7), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.left + 7, innerRect.top),
		BPoint(innerRect.right - 7, innerRect.top), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.left + 7, innerRect.bottom),
		BPoint(innerRect.right - 7, innerRect.bottom), B_MIXED_COLORS);

	innerRect.InsetBy(-1, -1);
	SetLowColor(0x58, 0x58, 0x58);
	SetHighColor(0x20, 0x20, 0x20);
	StrokeLine(BPoint(innerRect.left, innerRect.top + 8),
		BPoint(innerRect.left, innerRect.bottom - 8), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.right, innerRect.top + 8),
		BPoint(innerRect.right, innerRect.bottom - 8), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.left + 8, innerRect.top),
		BPoint(innerRect.right - 8, innerRect.top), B_MIXED_COLORS);
	StrokeLine(BPoint(innerRect.left + 8, innerRect.bottom),
		BPoint(innerRect.right - 8, innerRect.bottom), B_MIXED_COLORS);

	SetDrawingMode(B_OP_OVER);
}


void
NormalPulseView::Draw(BRect rect)
{
	PushState();

	SetDrawingMode(B_OP_OVER);
	// Processor picture
	DrawChip(fChipRect);

	// Baselines of the model and speed lines under the logo
	float processorY = fChipRect.top + 53;
	float speedY = fChipRect.top + 62;

	if (fBrandLogo != NULL) {
		DrawBitmap(fBrandLogo, BPoint(
			9 + (fChipRect.Width() - fBrandLogo->Bounds().Width()) / 2,
			fChipRect.top + 6));
	} else {
		// no logo for this vendor: the two text lines go to the bottom of the
		// chip body (DrawChip() fills fChipRect inset by 7) with a 3 pixel
		// margin, and the vendor name, as large as fits, centered above them
		font_height smallHeight;
		GetFontHeight(&smallHeight);
		speedY = fChipRect.bottom - 10 - ceilf(smallHeight.descent);
		processorY = speedY - ceilf(smallHeight.ascent + smallHeight.descent);
		float vendorTop = fChipRect.top + 10;
		float vendorBottom = processorY - ceilf(smallHeight.ascent) - 2;

		BFont font(be_bold_font);
		font.SetSize(18);
		float width = font.StringWidth(fVendor);
		font_height fontHeight;
		font.GetHeight(&fontHeight);
		float maxWidth = fChipRect.Width() - 16;
		float scale = std::min(maxWidth / width, (vendorBottom - vendorTop)
			/ (fontHeight.ascent + fontHeight.descent));
		if (scale < 1) {
			font.SetSize(std::max(8.0f, 18 * scale));
			width = font.StringWidth(fVendor);
			font.GetHeight(&fontHeight);
		}

		// Not PushState()/PopState(): PopState() leaves the bold font in the
		// view's cached state, so StringWidth() below would measure with it.
		BFont viewFont;
		GetFont(&viewFont);
		SetFont(&font);
		SetHighColor(240, 240, 240);
		MovePenTo(10 + (fChipRect.Width() - width) / 2,
			(vendorTop + vendorBottom + fontHeight.ascent - fontHeight.descent)
				/ 2);
		DrawString(fVendor);
		SetFont(&viewFont);
	}

	// Draw processor type and speed
	SetHighColor(240, 240, 240);

	float width = StringWidth(fProcessor);
	MovePenTo(10 + (fChipRect.Width() - width) / 2, processorY);
	DrawString(fProcessor);

	// The chip has room for one line under the model: the clock speed, or
	// the ISA level when the speed is unknown (arm64 has no clock register).
	char buffer[64];
	int32 cpuSpeed = get_rounded_cpu_speed();
	if (cpuSpeed <= 0)
		strlcpy(buffer, fIsa, sizeof(buffer));
	else if (cpuSpeed > 1000 && (cpuSpeed % 10) == 0)
		snprintf(buffer, sizeof(buffer), B_TRANSLATE("%.2f GHz"), cpuSpeed / 1000.0f);
	else
		snprintf(buffer, sizeof(buffer), B_TRANSLATE("%ld MHz"), (long int)cpuSpeed);

	// We can't assume anymore that a CPU clock speed is always static.
	// Let's compute the best font size for the CPU speed string each time...
	width = StringWidth(buffer);
	MovePenTo(10 + (fChipRect.Width() - width) / 2, speedY);
	DrawString(buffer);

	PopState();
}


void
NormalPulseView::Pulse()
{
	// Don't recalculate and redraw if this view is hidden
	if (!IsHidden()) {
		Update();
		if (Window()->Lock()) {
			// Set the value of each CPU bar
			for (int x = 0; x < fCpuCount; x++) {
				fProgressBars[x]->Set((int32)max_c(0, cpu_times[x] * 100));
			}

			Sync();
			Window()->Unlock();
		}
	}
}


void
NormalPulseView::AttachedToWindow()
{
	fPreviousTime = system_time();

	BMessenger messenger(Window());
	mode1->SetTarget(messenger);
	mode2->SetTarget(messenger);
	preferences->SetTarget(messenger);
	about->SetTarget(messenger);

	system_info sys_info;
	get_system_info(&sys_info);
	if (sys_info.cpu_count >= 2) {
		for (unsigned int x = 0; x < sys_info.cpu_count; x++)
			cpu_menu_items[x]->SetTarget(messenger);
	}
}


void
NormalPulseView::UpdateColors(BMessage *message)
{
	int32 color = message->FindInt32("color");
	bool fade = message->FindBool("fade");
	system_info sys_info;
	get_system_info(&sys_info);

	for (unsigned int x = 0; x < sys_info.cpu_count; x++) {
		fProgressBars[x]->UpdateColors(color, fade);
		fCpuButtons[x]->UpdateColors(color);
	}
}

