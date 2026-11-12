/*
 * Wordpad ribbon chrome (LeewayUI)
 *
 * Copyright 2026 Leeway Open Software
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define WIN32_LEAN_AND_MEAN

#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <richedit.h>
#include <commctrl.h>

#include <leewayui/ribbon.h>
#include <leewayui/ribbon_icons.h>
#include <leewayui/aureline_icons.h>

#include "wordpad.h"

static ILeewayRibbon *g_ribbon;
static HWND g_mainWnd;
static HWND g_zoomSlider;
static int g_sliderUpdating;
static UINT g_cfRichText;

#define TAB_HOME 1
#define TAB_VIEW 2
#define TAB_PRINTPREVIEW 3

/* Command table: labels/tooltips/keytips per RIBBON-APPS-SPEC.md section 3.
 * Icons stay at LWUI_ICON_NONE until the Aureline resolver lands (ribbon-f2). */
static LeewayUICommand ribbon_commands[] = {
    /* application menu */
    { ID_FILE_NEW,       L"New",              L"Create a new document",                 L"N",  LWUI_AUR_COMMON_NEW, TRUE,  FALSE, TRUE },
    { ID_FILE_OPEN,      L"Open",             L"Open an existing document",             L"O",  LWUI_AUR_COMMON_OPEN, TRUE,  FALSE, TRUE },
    { ID_FILE_SAVE,      L"Save",             L"Save the document",                     L"S",  LWUI_AUR_COMMON_SAVE, TRUE,  FALSE, TRUE },
    { ID_FILE_SAVEAS,    L"Save as",          L"Save the document with a new name or type", L"A", LWUI_AUR_COMMON_SAVEAS, TRUE, FALSE, TRUE },
    { ID_PRINT,          L"Print",            L"Print the document",                    L"P",  LWUI_AUR_COMMON_PRINT, TRUE,  FALSE, TRUE },
    { ID_PRINTSETUP,     L"Page setup",       L"Change page layout settings",           L"G",  LWUI_AUR_COMMON_PAGESETUP, TRUE,  FALSE, TRUE },
    { ID_VIEWPROPERTIES, L"Properties",       L"Document display properties",           L"I",  LWUI_AUR_COMMON_PROPERTIES, TRUE,  FALSE, TRUE },
    { ID_SEND_MAIL,      L"Send in e-mail",   L"Send a copy in an e-mail (not available yet)", L"D", LWUI_AUR_COMMON_EMAIL, FALSE, FALSE, TRUE },
    { ID_ABOUT,          L"About WordPad",    L"About WordPad",                         L"T",  LWUI_AUR_COMMON_ABOUT, TRUE,  FALSE, TRUE },
    { ID_FILE_EXIT,      L"Exit",             L"Exit WordPad",                          L"X",  LWUI_AUR_COMMON_EXIT, TRUE,  FALSE, TRUE },

    /* QAT */
    { ID_EDIT_UNDO,      L"Undo",             L"Undo (Ctrl+Z)",                         L"1",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_EDIT_REDO,      L"Redo",             L"Redo (Ctrl+Y)",                         L"2",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },

    /* clipboard */
    { ID_EDIT_PASTE,     L"Paste",            L"Paste (Ctrl+V)",                        L"V",  LWUI_AUR_COMMON_PASTE, TRUE,  FALSE, TRUE },
    { ID_EDIT_CUT,       L"Cut",              L"Cut (Ctrl+X)",                          L"X",  LWUI_AUR_COMMON_CUT, TRUE,  FALSE, TRUE },
    { ID_EDIT_COPY,      L"Copy",             L"Copy (Ctrl+C)",                         L"C",  LWUI_AUR_COMMON_COPY, TRUE,  FALSE, TRUE },
    { ID_EDIT_PASTESPECIAL, L"Paste special", L"Insert clipboard content with format choice", L"S", LWUI_AUR_COMMON_PASTE, FALSE, FALSE, TRUE },

    /* font */
    { ID_FONT_FAMILY,    L"Font",             L"Change the font",                       L"FF", LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_FONT_SIZE,      L"Size",             L"Change the font size",                  L"FS", LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_GROW_FONT,      L"Grow font",        L"Increase the font size",                L"FG", LWUI_AUR_WORDPAD_FONT_GROW, TRUE,  FALSE, TRUE },
    { ID_SHRINK_FONT,    L"Shrink font",      L"Decrease the font size",                L"FK", LWUI_AUR_WORDPAD_FONT_SHRINK, TRUE,  FALSE, TRUE },
    { ID_FORMAT_BOLD,    L"Bold",             L"Bold (Ctrl+B)",                         L"B",  LWUI_AUR_PAINT_BOLD, TRUE,  FALSE, TRUE },
    { ID_FORMAT_ITALIC,  L"Italic",           L"Italic (Ctrl+I)",                       L"I",  LWUI_AUR_PAINT_ITALIC, TRUE,  FALSE, TRUE },
    { ID_FORMAT_UNDERLINE, L"Underline",      L"Underline (Ctrl+U)",                    L"U",  LWUI_AUR_PAINT_UNDERLINE, TRUE,  FALSE, TRUE },
    { ID_FORMAT_STRIKE,  L"Strikethrough",    L"Strikethrough",                         L"4",  LWUI_AUR_WORDPAD_STRIKE, TRUE,  FALSE, TRUE },
    { ID_FORMAT_SUBSCRIPT, L"Subscript",      L"Subscript",                             L"5",  LWUI_AUR_WORDPAD_SUBSCRIPT, TRUE,  FALSE, TRUE },
    { ID_FORMAT_SUPERSCRIPT, L"Superscript",  L"Superscript",                           L"6",  LWUI_AUR_WORDPAD_SUPERSCRIPT, TRUE,  FALSE, TRUE },
    { ID_FORMAT_HIGHLIGHT, L"Text highlight colour", L"Mark text with a highlight colour", L"H", LWUI_AUR_WORDPAD_HIGHLIGHT, TRUE, FALSE, TRUE },
    { ID_FORMAT_COLOR,   L"Text colour",      L"Change the text colour",                L"FC", LWUI_AUR_WORDPAD_TEXT_COLOUR, TRUE,  FALSE, TRUE },
    { ID_FONTSETTINGS,   L"Font settings",    L"Open the font dialog",                  L"FN", LWUI_ICON_NONE, TRUE,  FALSE, TRUE },

    /* paragraph */
    { ID_INDENT_DECREASE, L"Decrease indent", L"Decrease the indent",                   L"AO", LWUI_AUR_WORDPAD_INDENT_LESS, TRUE,  FALSE, TRUE },
    { ID_INDENT_INCREASE, L"Increase indent", L"Increase the indent",                   L"AI", LWUI_AUR_WORDPAD_INDENT_MORE, TRUE,  FALSE, TRUE },
    { ID_BULLETONOFF,    L"Bullets",          L"Start a list",                          L"N",  LWUI_AUR_WORDPAD_LIST, TRUE,  FALSE, TRUE },
    { ID_LINESPACING,    L"Line spacing",     L"Choose the line spacing",               L"K",  LWUI_AUR_WORDPAD_LINE_SPACING, TRUE,  FALSE, TRUE },
    { ID_ALIGN_LEFT,     L"Align left",       L"Align left (Ctrl+L)",                   L"AL", LWUI_AUR_WORDPAD_ALIGN_LEFT, TRUE,  FALSE, TRUE },
    { ID_ALIGN_CENTER,   L"Centre",           L"Centre text (Ctrl+E)",                  L"AC", LWUI_AUR_WORDPAD_ALIGN_CENTRE, TRUE,  FALSE, TRUE },
    { ID_ALIGN_RIGHT,    L"Align right",      L"Align right (Ctrl+R)",                  L"AR", LWUI_AUR_WORDPAD_ALIGN_RIGHT, TRUE,  FALSE, TRUE },
    { ID_ALIGN_JUSTIFY,  L"Justify",          L"Justify text",                          L"AJ", LWUI_AUR_WORDPAD_ALIGN_JUSTIFY, TRUE,  FALSE, TRUE },
    { ID_PARAFORMAT,     L"Paragraph settings", L"Open the paragraph dialog",           NULL,  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_LINESPACING_100, L"1.0",             L"Single line spacing",                   L"1",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_LINESPACING_115, L"1.15",            L"Line spacing 1.15",                     L"2",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_LINESPACING_150, L"1.5",             L"Line spacing 1.5",                      L"3",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_LINESPACING_200, L"2.0",             L"Double line spacing",                   L"4",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },

    /* insert */
    { ID_DATETIME,       L"Date and time",    L"Insert the current date and time",      L"D",  LWUI_AUR_WORDPAD_DATETIME, TRUE,  FALSE, TRUE },
    { ID_INSERT_OBJECT,  L"Insert object",    L"Embed an object (not available yet)",   L"J",  LWUI_AUR_WORDPAD_OBJECT, FALSE, FALSE, TRUE },
    { ID_INSERT_PICTURE, L"Picture",          L"Insert a picture from a file (not available yet)", L"P", LWUI_AUR_WORDPAD_PICTURE, FALSE, FALSE, TRUE },

    /* editing */
    { ID_FIND,           L"Find",             L"Find text (Ctrl+F)",                    L"FD", LWUI_AUR_COMMON_FIND, TRUE,  FALSE, TRUE },
    { ID_REPLACE,        L"Replace",          L"Replace text (Ctrl+H)",                 L"R",  LWUI_AUR_COMMON_REPLACE, TRUE,  FALSE, TRUE },
    { ID_EDIT_SELECTALL, L"Select all",       L"Select all (Ctrl+A)",                   L"SL", LWUI_AUR_COMMON_SELECT_ALL, TRUE,  FALSE, TRUE },

    /* view */
    { ID_ZOOM_IN,        L"Zoom in",          L"Zoom in",                               L"I",  LWUI_AUR_COMMON_ZOOM_IN, TRUE,  FALSE, TRUE },
    { ID_ZOOM_OUT,       L"Zoom out",         L"Zoom out",                              L"O",  LWUI_AUR_COMMON_ZOOM_OUT, TRUE,  FALSE, TRUE },
    { ID_ZOOM_100,       L"100%",             L"Zoom to 100%",                          L"1",  LWUI_AUR_COMMON_ZOOM, TRUE,  FALSE, TRUE },
    { ID_TOGGLE_RULER,   L"Ruler",            L"Show or hide the ruler",                L"R",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_TOGGLE_STATUSBAR, L"Status bar",     L"Show or hide the status bar",           L"S",  LWUI_AUR_COMMON_STATUSBAR, TRUE,  FALSE, TRUE },
    { ID_WORDWRAP_OPTIONS, L"Word wrap",      L"Choose how text wraps",                 L"W",  LWUI_AUR_WORDPAD_WRAP, TRUE,  FALSE, TRUE },
    { ID_MEASUREMENT_UNITS, L"Measurement units", L"Units for the ruler",              L"M",  LWUI_AUR_WORDPAD_UNITS, TRUE,  FALSE, TRUE },
    { ID_WORDWRAP_OPT_NONE,   L"No wrap",         L"Do not wrap text",                  L"1",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_WORDWRAP_OPT_WINDOW, L"Wrap to window",  L"Wrap text to the window",           L"2",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_WORDWRAP_OPT_MARGIN, L"Wrap to ruler",   L"Wrap text at the ruler margin",     L"3",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_UNITS_INCHES,      L"Inches",           L"Measure in inches",                 L"1",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_UNITS_CENTIMETERS, L"Centimeters",      L"Measure in centimeters",            L"2",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_UNITS_POINTS,      L"Points",           L"Measure in points",                 L"3",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },
    { ID_UNITS_PICAS,       L"Picas",            L"Measure in picas",                  L"4",  LWUI_ICON_NONE, TRUE,  FALSE, TRUE },

    /* print preview (contextual tab) */
    { ID_PREVIEW_ZOOMIN,  L"Zoom in",          L"Zoom in",                               L"I",  LWUI_AUR_COMMON_ZOOM_IN, TRUE,  FALSE, TRUE },
    { ID_PREVIEW_ZOOMOUT, L"Zoom out",         L"Zoom out",                              L"O",  LWUI_AUR_COMMON_ZOOM_OUT, TRUE,  FALSE, TRUE },
    { ID_PREVIEW_NUMPAGES, L"One page / two pages", L"Switch between one and two pages", L"T",  LWUI_AUR_WORDPAD_TWO_PAGES, TRUE,  FALSE, TRUE },
    { ID_PREVIEW_PREVPAGE, L"Previous page",  L"Previous page",                         L"V",  LWUI_AUR_VIEWER_PREV, TRUE,  FALSE, TRUE },
    { ID_PREVIEW_NEXTPAGE, L"Next page",      L"Next page",                             L"N",  LWUI_AUR_VIEWER_NEXT, TRUE,  FALSE, TRUE },
    { ID_CLOSE_PREVIEW,   L"Close print preview", L"Close print preview",                L"C",  LWUI_AUR_COMMON_CLOSE, TRUE,  FALSE, TRUE },

    /* text colour collection (also used by highlight) */
    { 1800, L"Black",     NULL, L"1", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1801, L"Maroon",    NULL, L"2", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1802, L"Green",     NULL, L"3", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1803, L"Olive",     NULL, L"4", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1804, L"Navy",      NULL, L"5", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1805, L"Purple",    NULL, L"6", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1806, L"Teal",      NULL, L"7", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1807, L"Gray",      NULL, L"8", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1808, L"Silver",    NULL, L"9", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1809, L"Red",       NULL, L"0", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1810, L"Lime",      NULL, L"A", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1811, L"Yellow",    NULL, L"B", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1812, L"Blue",      NULL, L"C", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1813, L"Fuchsia",   NULL, L"D", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1814, L"Aqua",      NULL, L"E", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1815, L"White",     NULL, L"F", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
    { 1816, L"Automatic", NULL, L"M", LWUI_ICON_NONE, TRUE, FALSE, TRUE },
};

/* matches wordpad.c textcolors[] */
static const COLORREF ribbon_textcolors[] =
{
    0x00000000L, 0x00000080L, 0x00008000L, 0x00008080L,
    0x00800000L, 0x00800080L, 0x00808000L, 0x00808080L,
    0x00c0c0c0L, 0x000000ffL, 0x0000ff00L, 0x0000ffffL,
    0x00ff0000L, 0x00ff00ffL, 0x00ffff00L, 0x00ffffffL
};

/* ---- layout ---- */

static const UINT ribbon_appmenu[] = {
    ID_FILE_NEW, ID_FILE_OPEN, ID_FILE_SAVE, ID_FILE_SAVEAS,
    ID_PRINT, ID_PRINTSETUP, ID_VIEWPROPERTIES, ID_SEND_MAIL,
    ID_ABOUT, ID_FILE_EXIT
};

static const UINT ribbon_qat[] = { ID_FILE_SAVE, ID_EDIT_UNDO, ID_EDIT_REDO };

static const LeewayUIPlacement ribbon_clipboard_controls[] = {
    { ID_EDIT_PASTE,     LWUI_CONTROL_SPLIT_LARGE, NULL, 0 },
    { ID_EDIT_CUT,       LWUI_CONTROL_BUTTON_SMALL,  NULL, 0 },
    { ID_EDIT_COPY,      LWUI_CONTROL_BUTTON_SMALL,  NULL, 0 },
};

static const LeewayUIPlacement ribbon_font_controls[] = {
    { ID_FONT_FAMILY,    LWUI_CONTROL_COMBO,  L"FF", 150 },
    { ID_FONT_SIZE,      LWUI_CONTROL_COMBO,  L"FS", 55 },
    { ID_GROW_FONT,      LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_SHRINK_FONT,    LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_FORMAT_BOLD,    LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_FORMAT_ITALIC,  LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_FORMAT_UNDERLINE, LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_FORMAT_STRIKE,  LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_FORMAT_SUBSCRIPT, LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_FORMAT_SUPERSCRIPT, LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_FORMAT_HIGHLIGHT, LWUI_CONTROL_SPLIT_SMALL, NULL, 0 },
    { ID_FORMAT_COLOR,   LWUI_CONTROL_SPLIT_SMALL, NULL, 0 },
};

static const LeewayUIPlacement ribbon_paragraph_controls[] = {
    { ID_INDENT_DECREASE, LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_INDENT_INCREASE, LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_BULLETONOFF,    LWUI_CONTROL_SPLIT_SMALL, NULL, 0 },
    { ID_LINESPACING,    LWUI_CONTROL_DROPDOWN,    NULL, 0 },
    { ID_ALIGN_LEFT,     LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_ALIGN_CENTER,   LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_ALIGN_RIGHT,    LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
    { ID_ALIGN_JUSTIFY,  LWUI_CONTROL_BUTTON_SMALL_ICON, NULL, 0 },
};

static const LeewayUIPlacement ribbon_insert_controls[] = {
    { ID_DATETIME,       LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
    { ID_INSERT_OBJECT,  LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
    { ID_INSERT_PICTURE, LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
};

static const LeewayUIPlacement ribbon_editing_controls[] = {
    { ID_FIND,           LWUI_CONTROL_BUTTON_LARGE, L"FD", 0 },
    { ID_REPLACE,        LWUI_CONTROL_BUTTON_SMALL, NULL, 0 },
    { ID_EDIT_SELECTALL, LWUI_CONTROL_BUTTON_SMALL, NULL, 0 },
};

static const LeewayUIPlacement ribbon_zoom_controls[] = {
    { ID_ZOOM_IN,        LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
    { ID_ZOOM_OUT,       LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
    { ID_ZOOM_100,       LWUI_CONTROL_BUTTON_SMALL, NULL, 0 },
};

static const LeewayUIPlacement ribbon_show_controls[] = {
    { ID_TOGGLE_RULER,     LWUI_CONTROL_CHECKBOX, NULL, 0 },
    { ID_TOGGLE_STATUSBAR, LWUI_CONTROL_CHECKBOX, NULL, 0 },
};

static const LeewayUIPlacement ribbon_settings_controls[] = {
    { ID_WORDWRAP_OPTIONS,  LWUI_CONTROL_DROPDOWN, NULL, 0 },
    { ID_MEASUREMENT_UNITS, LWUI_CONTROL_DROPDOWN, NULL, 0 },
};

static const LeewayUIPlacement ribbon_pv_print_controls[] = {
    { ID_PRINT,     LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
    { ID_PRINTSETUP, LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
};

static const LeewayUIPlacement ribbon_pv_zoom_controls[] = {
    { ID_PREVIEW_ZOOMIN,  LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
    { ID_PREVIEW_ZOOMOUT, LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
    { ID_PREVIEW_NUMPAGES, LWUI_CONTROL_TOGGLE_SMALL, NULL, 0 },
};

static const LeewayUIPlacement ribbon_pv_page_controls[] = {
    { ID_PREVIEW_PREVPAGE, LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
    { ID_PREVIEW_NEXTPAGE, LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
    { ID_CLOSE_PREVIEW,     LWUI_CONTROL_BUTTON_LARGE, NULL, 0 },
};

static const LeewayUIGroup ribbon_home_groups[] = {
    { L"Clipboard", NULL, 0, 0, ARRAY_SIZE(ribbon_clipboard_controls), ribbon_clipboard_controls },
    { L"Font", L"FO", 0, ID_FONTSETTINGS, ARRAY_SIZE(ribbon_font_controls), ribbon_font_controls },
    { L"Paragraph", L"GR", 0, ID_PARAFORMAT, ARRAY_SIZE(ribbon_paragraph_controls), ribbon_paragraph_controls },
    { L"Insert", NULL, 0, 0, ARRAY_SIZE(ribbon_insert_controls), ribbon_insert_controls },
    { L"Editing", L"ED", 0, 0, ARRAY_SIZE(ribbon_editing_controls), ribbon_editing_controls },
};

static const LeewayUIGroup ribbon_view_groups[] = {
    { L"Zoom", L"ZM", 0, 0, ARRAY_SIZE(ribbon_zoom_controls), ribbon_zoom_controls },
    { L"Show or hide", L"HW", 0, 0, ARRAY_SIZE(ribbon_show_controls), ribbon_show_controls },
    { L"Settings", L"TS", 0, 0, ARRAY_SIZE(ribbon_settings_controls), ribbon_settings_controls },
};

static const LeewayUIGroup ribbon_preview_groups[] = {
    { L"Print", NULL, 0, 0, ARRAY_SIZE(ribbon_pv_print_controls), ribbon_pv_print_controls },
    { L"Zoom", L"ZM", 0, 0, ARRAY_SIZE(ribbon_pv_zoom_controls), ribbon_pv_zoom_controls },
    { L"Preview", NULL, 0, 0, ARRAY_SIZE(ribbon_pv_page_controls), ribbon_pv_page_controls },
};

static const LeewayUITab ribbon_tabs[] = {
    { TAB_HOME,         L"Home",          L"H", NULL,            ARRAY_SIZE(ribbon_home_groups),     ribbon_home_groups },
    { TAB_VIEW,         L"View",          L"V", NULL,            ARRAY_SIZE(ribbon_view_groups),     ribbon_view_groups },
    { TAB_PRINTPREVIEW, L"Print preview", L"P", L"PrintPreview", ARRAY_SIZE(ribbon_preview_groups),  ribbon_preview_groups },
};

static const LeewayUILayout ribbon_layout = {
    L"WordPad",
    ARRAY_SIZE(ribbon_tabs), ribbon_tabs,
    ARRAY_SIZE(ribbon_appmenu), ribbon_appmenu,
    ARRAY_SIZE(ribbon_qat), ribbon_qat,
    ID_ABOUT
};

/* ---- collections ---- */

static void ribbon_set_colour_collection(UINT32 command)
{
    static const WCHAR *colour_labels[17] = {
        L"Black", L"Maroon", L"Green", L"Olive", L"Navy", L"Purple", L"Teal", L"Gray",
        L"Silver", L"Red", L"Lime", L"Yellow", L"Blue", L"Fuchsia", L"Aqua", L"White",
        L"Automatic"
    };
    LeewayUICollectionItem items[17];
    UINT i;

    ZeroMemory(items, sizeof(items));
    for (i = 0; i < 17; i++)
    {
        items[i].id = 1800 + i;
        items[i].label = colour_labels[i];
        items[i].color = (i < 16) ? ribbon_textcolors[i] : LWUI_COLOR_NONE;
    }
    LeewayRibbonSetCollection(g_ribbon, command, items, 17, LWUI_COLLECTION_COLORS);
}

static void ribbon_set_static_collections(void)
{
    static const LeewayUICollectionItem saveas[] = {
        { ID_SAVEAS_RICHTEXT, L"Rich Text" },
        { ID_SAVEAS_TEXT,     L"Plain text" },
        { ID_SAVEAS_OTHER,    L"Other formats" },
    };
    static const LeewayUICollectionItem print[] = {
        { ID_PRINT,       L"Print" },
        { ID_PRINT_QUICK, L"Quick print" },
        { ID_PREVIEW,     L"Print preview" },
    };
    static const LeewayUICollectionItem paste[] = {
        { ID_EDIT_PASTE,        L"Paste" },
        { ID_EDIT_PASTESPECIAL, L"Paste special..." },
    };
    static const LeewayUICollectionItem list[] = {
        { ID_BULLET,       L"Bullets" },
        { ID_BULLET + 1,   L"Numbering" },
        { ID_BULLET + 2,   L"Lower-case letters" },
        { ID_BULLET + 3,   L"Upper-case letters" },
        { ID_BULLET + 4,   L"Lower-case roman" },
        { ID_BULLET + 5,   L"Upper-case roman" },
    };
    static const LeewayUICollectionItem spacing[] = {
        { ID_LINESPACING_100, L"1.0" },
        { ID_LINESPACING_115, L"1.15" },
        { ID_LINESPACING_150, L"1.5" },
        { ID_LINESPACING_200, L"2.0" },
    };
    static const LeewayUICollectionItem wrap[] = {
        { ID_WORDWRAP_OPT_NONE,   L"No wrap" },
        { ID_WORDWRAP_OPT_WINDOW, L"Wrap to window" },
        { ID_WORDWRAP_OPT_MARGIN, L"Wrap to ruler" },
    };
    static const LeewayUICollectionItem units[] = {
        { ID_UNITS_INCHES,      L"Inches" },
        { ID_UNITS_CENTIMETERS, L"Centimeters" },
        { ID_UNITS_POINTS,      L"Points" },
        { ID_UNITS_PICAS,       L"Picas" },
    };

    LeewayRibbonSetCollection(g_ribbon, ID_FILE_SAVEAS, saveas, ARRAY_SIZE(saveas), 0);
    LeewayRibbonSetCollection(g_ribbon, ID_PRINT, print, ARRAY_SIZE(print), 0);
    LeewayRibbonSetCollection(g_ribbon, ID_EDIT_PASTE, paste, ARRAY_SIZE(paste), 0);
    LeewayRibbonSetCollection(g_ribbon, ID_BULLETONOFF, list, ARRAY_SIZE(list), 0);
    LeewayRibbonSetCollection(g_ribbon, ID_LINESPACING, spacing, ARRAY_SIZE(spacing), 0);
    LeewayRibbonSetCollection(g_ribbon, ID_WORDWRAP_OPTIONS, wrap, ARRAY_SIZE(wrap), 0);
    LeewayRibbonSetCollection(g_ribbon, ID_MEASUREMENT_UNITS, units, ARRAY_SIZE(units), 0);
    ribbon_set_colour_collection(ID_FORMAT_COLOR);
    ribbon_set_colour_collection(ID_FORMAT_HIGHLIGHT);
}

/* ---- font and size combos ---- */

struct font_enum_data
{
    WCHAR **names;
    int count;
    int capacity;
};

static int ribbon_name_index(struct font_enum_data *data, LPCWSTR name)
{
    int i;
    for (i = 0; i < data->count; i++)
        if (!lstrcmpiW(data->names[i], name))
            return i;
    return -1;
}

static int CALLBACK ribbon_font_enum_proc(const LOGFONTW *lpe, const TEXTMETRICW *lpnte,
                                          DWORD fontType, LPARAM lParam)
{
    struct font_enum_data *data = (struct font_enum_data *)lParam;

    if (lpe->lfFaceName[0] == L'@')
        return 1;
    if (ribbon_name_index(data, lpe->lfFaceName) >= 0)
        return 1;
    if (data->count == data->capacity)
    {
        int newcap = data->capacity ? data->capacity * 2 : 64;
        WCHAR **newnames = (WCHAR **)realloc(data->names, newcap * sizeof(WCHAR *));
        if (!newnames)
            return 0;
        data->names = newnames;
        data->capacity = newcap;
    }
    data->names[data->count] = _wcsdup(lpe->lfFaceName);
    if (!data->names[data->count])
        return 0;
    data->count++;
    return 1;
}

static int CALLBACK ribbon_sort_names(const void *a, const void *b)
{
    return lstrcmpiW(*(const WCHAR *const *)a, *(const WCHAR *const *)b);
}

static void ribbon_build_font_combo(void)
{
    struct font_enum_data data;
    LeewayUICollectionItem *items;
    HDC hdc;
    int i;

    ZeroMemory(&data, sizeof(data));
    hdc = GetDC(NULL);
    EnumFontFamiliesExW(hdc, NULL, ribbon_font_enum_proc, (LPARAM)&data, 0);
    ReleaseDC(NULL, hdc);
    qsort(data.names, data.count, sizeof(WCHAR *), ribbon_sort_names);

    items = (LeewayUICollectionItem *)calloc(data.count, sizeof(*items));
    if (items)
    {
        for (i = 0; i < data.count; i++)
        {
            items[i].id = 0;
            items[i].label = data.names[i];
        }
        LeewayRibbonSetCollection(g_ribbon, ID_FONT_FAMILY, items, data.count, 0);
        free(items);
    }
    for (i = 0; i < data.count; i++)
        free(data.names[i]);
    free(data.names);
}

static void ribbon_build_size_combo(void)
{
    static const unsigned sizes[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };
    LeewayUICollectionItem items[ARRAY_SIZE(sizes)];
    WCHAR labels[ARRAY_SIZE(sizes)][4];
    UINT i;

    ZeroMemory(items, sizeof(items));
    for (i = 0; i < ARRAY_SIZE(sizes); i++)
    {
        swprintf(labels[i], ARRAY_SIZE(labels[i]), L"%u", sizes[i]);
        items[i].id = 0;
        items[i].label = labels[i];
    }
    LeewayRibbonSetCollection(g_ribbon, ID_FONT_SIZE, items, ARRAY_SIZE(items), 0);
}

/* ---- state helpers ---- */

static HWND ribbon_editor(void)
{
    return GetDlgItem(g_mainWnd, IDC_EDITOR);
}

static BOOL ribbon_id_in_preview_tab(UINT32 id)
{
    return id == ID_PRINT || id == ID_PRINTSETUP ||
           id == ID_PREVIEW_ZOOMIN || id == ID_PREVIEW_ZOOMOUT || id == ID_PREVIEW_NUMPAGES ||
           id == ID_PREVIEW_PREVPAGE || id == ID_PREVIEW_NEXTPAGE || id == ID_CLOSE_PREVIEW;
}

static void ribbon_recent_label(int index, WCHAR *buffer, UINT cch)
{
    LPCWSTR path = registry_get_recent_file(index);
    LPCWSTR name;

    buffer[0] = L'\0';
    if (!path || !path[0])
        return;
    name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    lstrcpynW(buffer, name, cch);
}

static HRESULT CALLBACK ribbon_query_state(UINT32 id, LeewayUICommandState *state, void *ctx)
{
    static WCHAR comboLabel[64];
    HWND editor = ribbon_editor();
    BOOL preview = preview_isactive();
    CHARFORMAT2W fmt;
    PARAFORMAT2 pf;

    if (preview)
    {
        state->enabled = ribbon_id_in_preview_tab(id);
        return S_OK;
    }

    ZeroMemory(&fmt, sizeof(fmt));
    fmt.cbSize = sizeof(fmt);
    SendMessageW(editor, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&fmt);
    ZeroMemory(&pf, sizeof(pf));
    pf.cbSize = sizeof(pf);
    SendMessageW(editor, EM_GETPARAFORMAT, 0, (LPARAM)&pf);

    switch (id)
    {
        case ID_EDIT_UNDO:
        case ID_EDIT_REDO:
            break;
        case ID_EDIT_CUT:
        case ID_EDIT_COPY:
        {
            LONG from, to;
            SendMessageW(editor, EM_GETSEL, (WPARAM)&from, (LPARAM)&to);
            state->enabled = (from != to);
            break;
        }
        case ID_EDIT_PASTE:
            state->enabled = IsClipboardFormatAvailable(CF_UNICODETEXT) ||
                             IsClipboardFormatAvailable(CF_TEXT) ||
                             (g_cfRichText && IsClipboardFormatAvailable(g_cfRichText));
            break;
        case ID_FIND:
        case ID_FIND_NEXT:
        {
            GETTEXTLENGTHEX gt;
            gt.flags = GTL_NUMCHARS;
            gt.codepage = 1200;
            state->enabled = SendMessageW(editor, EM_GETTEXTLENGTHEX, (WPARAM)&gt, 0) != 0;
            break;
        }
        case ID_FORMAT_BOLD:
            state->checked = (fmt.dwMask & CFM_BOLD) && (fmt.dwEffects & CFE_BOLD);
            break;
        case ID_FORMAT_ITALIC:
            state->checked = (fmt.dwMask & CFM_ITALIC) && (fmt.dwEffects & CFE_ITALIC);
            break;
        case ID_FORMAT_UNDERLINE:
            state->checked = (fmt.dwMask & CFM_UNDERLINE) && (fmt.dwEffects & CFE_UNDERLINE);
            break;
        case ID_FORMAT_STRIKE:
            state->checked = (fmt.dwMask & CFM_STRIKEOUT) && (fmt.dwEffects & CFE_STRIKEOUT);
            break;
        case ID_FORMAT_SUBSCRIPT:
            state->checked = (fmt.dwMask & CFM_SUBSCRIPT) && (fmt.dwEffects & CFE_SUBSCRIPT);
            break;
        case ID_FORMAT_SUPERSCRIPT:
            state->checked = (fmt.dwMask & CFM_SUPERSCRIPT) && (fmt.dwEffects & CFE_SUPERSCRIPT);
            break;
        case ID_FORMAT_COLOR:
            if ((fmt.dwMask & CFM_COLOR) && !(fmt.dwEffects & CFE_AUTOCOLOR))
                state->color = fmt.crTextColor;
            break;
        case ID_FORMAT_HIGHLIGHT:
            if ((fmt.dwMask & CFM_BACKCOLOR) && !(fmt.dwEffects & CFE_AUTOBACKCOLOR))
                state->color = fmt.crBackColor;
            break;
        case ID_ALIGN_LEFT:
            state->checked = (pf.wAlignment == PFA_LEFT);
            break;
        case ID_ALIGN_CENTER:
            state->checked = (pf.wAlignment == PFA_CENTER);
            break;
        case ID_ALIGN_RIGHT:
            state->checked = (pf.wAlignment == PFA_RIGHT);
            break;
        case ID_ALIGN_JUSTIFY:
            state->checked = (pf.wAlignment == PFA_JUSTIFY);
            break;
        case ID_BULLETONOFF:
            state->checked = (pf.wNumbering != 0);
            state->enabled = !wordpad_is_readonly();
            break;
        case ID_ZOOM_100:
            state->checked = (wordpad_get_zoom() == 100);
            break;
        case ID_TOGGLE_RULER:
        case ID_TOGGLE_STATUSBAR:
        case LWUI_STATUSBAR_TOGGLE:
            break;
        case ID_WORDWRAP_OPT_NONE:
        case ID_WORDWRAP_OPT_WINDOW:
        case ID_WORDWRAP_OPT_MARGIN:
            state->checked = wordpad_get_word_wrap_option() == (int)(id - ID_WORDWRAP_OPT_NONE);
            state->enabled = !wordpad_is_readonly();
            break;
        case ID_UNITS_INCHES:
        case ID_UNITS_CENTIMETERS:
        case ID_UNITS_POINTS:
        case ID_UNITS_PICAS:
            state->checked = (get_ruler_unit() == (int)(id - ID_UNITS_INCHES));
            break;
        case ID_FONT_FAMILY:
            lstrcpynW(comboLabel, fmt.szFaceName, ARRAY_SIZE(comboLabel));
            state->label = comboLabel;
            state->enabled = !wordpad_is_readonly();
            break;
        case ID_FONT_SIZE:
            swprintf(comboLabel, ARRAY_SIZE(comboLabel), L"%d", fmt.yHeight / 20);
            state->label = comboLabel;
            state->enabled = !wordpad_is_readonly();
            break;
        case ID_GROW_FONT:
        case ID_SHRINK_FONT:
        case ID_INDENT_DECREASE:
        case ID_INDENT_INCREASE:
        case ID_LINESPACING:
        case ID_LINESPACING_100:
        case ID_LINESPACING_115:
        case ID_LINESPACING_150:
        case ID_LINESPACING_200:
        case ID_FONTSETTINGS:
        case ID_PARAFORMAT:
            state->enabled = !wordpad_is_readonly();
            break;
    }
    return S_OK;
}

static HRESULT CALLBACK ribbon_execute(UINT32 id, void *ctx)
{
    WCHAR text[128];
    UINT cch;

    switch (id)
    {
        case ID_FONT_FAMILY:
        case ID_FONT_SIZE:
            cch = ARRAY_SIZE(text);
            if (SUCCEEDED(LeewayRibbonGetComboText(g_ribbon, id, text, &cch)))
            {
                if (id == ID_FONT_FAMILY)
                    wordpad_apply_font_face(text);
                else
                    wordpad_apply_font_size(text);
            }
            return S_OK;
        case ID_FILE_RECENT1:
        case ID_FILE_RECENT2:
        case ID_FILE_RECENT3:
        case ID_FILE_RECENT4:
            wordpad_open_recent_file(id - ID_FILE_RECENT1);
            return S_OK;
        case ID_CLOSE_PREVIEW:
            preview_exit(g_mainWnd);
            return S_OK;
        case ID_WORDWRAP_OPT_NONE:
        case ID_WORDWRAP_OPT_WINDOW:
        case ID_WORDWRAP_OPT_MARGIN:
            wordpad_set_word_wrap(id - ID_WORDWRAP_OPT_NONE);
            return S_OK;
        case ID_UNITS_INCHES:
        case ID_UNITS_CENTIMETERS:
        case ID_UNITS_POINTS:
        case ID_UNITS_PICAS:
            set_ruler_unit(id - ID_UNITS_INCHES);
            LeewayRibbonInvalidateCommand(g_ribbon, ID_MEASUREMENT_UNITS);
            return S_OK;
        case LWUI_STATUSBAR_TOGGLE:
            SendMessageW(g_mainWnd, WM_COMMAND, ID_TOGGLE_STATUSBAR, 0);
            return S_OK;
        case ID_ZOOM_IN:
        case ID_ZOOM_OUT:
        case ID_ZOOM_100:
            SendMessageW(g_mainWnd, WM_COMMAND, id, 0);
            ribbon_sync_zoom();
            return S_OK;
    }

    /* everything else routes to the existing command dispatch */
    SendMessageW(g_mainWnd, WM_COMMAND, id, 0);
    return S_OK;
}

static void CALLBACK ribbon_recent_open(const WCHAR *path, void *context)
{
    (void)context;
    if (path && path[0])
        wordpad_open_file_path(path);
}

void ribbon_add_recent_file(LPCWSTR path)
{
    if (g_ribbon && path && path[0])
        LeewayRibbonAddRecentFile(g_ribbon, path);
}

/* ---- status bar zoom slider ---- */

void ribbon_layout_statusbar(HWND hStatusBar)
{
    RECT rcMain, rcStatus, rcPart;
    POINT pt;
    INT parts[3];
    int sliderWidth = 190;
    int pctWidth = 56;
    int statusHeight;

    if (!hStatusBar)
        return;

    GetClientRect(g_mainWnd, &rcMain);
    GetWindowRect(hStatusBar, &rcStatus);
    pt.x = rcStatus.left;
    pt.y = rcStatus.top;
    ScreenToClient(g_mainWnd, &pt);
    statusHeight = rcStatus.bottom - rcStatus.top;

    parts[0] = rcMain.right - sliderWidth - pctWidth - 16;
    parts[1] = parts[0] + pctWidth;
    parts[2] = -1;
    SendMessageW(hStatusBar, SB_SETPARTS, 3, (LPARAM)parts);

    if (!g_zoomSlider)
        return;

    SendMessageW(hStatusBar, SB_GETRECT, 2, (LPARAM)&rcPart);
    MoveWindow(g_zoomSlider, rcPart.left + 4, pt.y + 2,
               rcPart.right - rcPart.left - 8, statusHeight - 4, TRUE);

    ribbon_sync_zoom();
}

void ribbon_sync_zoom(void)
{
    int pct = wordpad_get_zoom();
    WCHAR buffer[16];
    HWND hStatusBar = GetDlgItem(g_mainWnd, IDC_STATUSBAR);

    if (g_zoomSlider)
    {
        g_sliderUpdating = 1;
        SendMessageW(g_zoomSlider, TBM_SETPOS, TRUE, pct);
        g_sliderUpdating = 0;
    }
    if (hStatusBar)
    {
        swprintf(buffer, ARRAY_SIZE(buffer), L"%d%%", pct);
        SendMessageW(hStatusBar, SB_SETTEXTW, 1, (LPARAM)buffer);
    }
}

BOOL ribbon_on_hscroll(HWND hwndCtl, int code)
{
    int pct;

    if (hwndCtl != g_zoomSlider)
        return FALSE;
    if (g_sliderUpdating)
        return TRUE;

    pct = (int)SendMessageW(g_zoomSlider, TBM_GETPOS, 0, 0);
    SendMessageW(ribbon_editor(), EM_SETZOOM, pct, 100);
    LeewayRibbonInvalidateCommand(g_ribbon, ID_ZOOM_100);
    ribbon_sync_zoom();
    return TRUE;
}

/* ---- lifetime ---- */

BOOL ribbon_create(HWND hMainWnd)
{
    HINSTANCE hInstance = GetModuleHandleW(0);
    HACCEL hAccel;
    g_mainWnd = hMainWnd;
    g_cfRichText = RegisterClipboardFormatW(L"Rich Text Format");

    if (FAILED(LeewayRibbonCreate(hMainWnd, ribbon_commands, ARRAY_SIZE(ribbon_commands),
                                  ribbon_execute, ribbon_query_state, NULL, &g_ribbon)))
        return FALSE;

    if (FAILED(LeewayRibbonSetLayout(g_ribbon, &ribbon_layout)))
        return FALSE;

    ribbon_set_static_collections();
    ribbon_build_font_combo();
    ribbon_build_size_combo();

    hAccel = LoadAcceleratorsW(hInstance, L"MAINACCELTABLE");
    if (hAccel)
        LeewayRibbonSetAccelerators(g_ribbon, hAccel);

    g_zoomSlider = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0, 0, 100, 20, hMainWnd, NULL, hInstance, NULL);
    if (g_zoomSlider)
    {
        SendMessageW(g_zoomSlider, TBM_SETRANGE, TRUE, MAKELPARAM(10, 500));
        SendMessageW(g_zoomSlider, TBM_SETLINESIZE, 0, 10);
        SendMessageW(g_zoomSlider, TBM_SETPAGESIZE, 0, 50);
    }

    /* Aureline icons: assets ship in the tree next to the framework */
    {
        static LeewayAurelineConfig aurcfg;
        static WCHAR iconroot[MAX_PATH];
        WCHAR *slash;

        GetModuleFileNameW(NULL, iconroot, MAX_PATH);
        slash = wcsrchr(iconroot, L'\\');
        if (slash)
            *slash = L'\0';
        lstrcatW(iconroot, L"\\..\\..\\..\\..\\sdk\\lib\\leewayui\\icons");
        aurcfg.root = iconroot;
        aurcfg.mono_fallback_to_colour = TRUE;
        LeewayRibbonUseAurelineIcons(g_ribbon, &aurcfg);
    }

    /* recent files: framework right pane, backed by our existing MRU */
        LeewayRibbonSetRecentProvider(g_ribbon, ribbon_recent_open, NULL, FILELIST_ENTRIES);
    {
        int i;
        for (i = 0; i < FILELIST_ENTRIES; i++)
        {
            LPCWSTR path = registry_get_recent_file(i);
            if (path && path[0])
                LeewayRibbonAddRecentFile(g_ribbon, path);
        }
    }

    LeewayRibbonSetContext(g_ribbon, L"PrintPreview", FALSE);
    return TRUE;
}

void ribbon_destroy(void)
{
    if (g_ribbon)
    {
        LeewayRibbonDestroy(g_ribbon);
        g_ribbon = NULL;
    }
    if (g_zoomSlider)
    {
        DestroyWindow(g_zoomSlider);
        g_zoomSlider = NULL;
    }
    g_mainWnd = NULL;
}

UINT ribbon_height(void)
{
    return g_ribbon ? LeewayRibbonGetHeight(g_ribbon) : 0;
}

HWND ribbon_hwnd(void)
{
    return g_ribbon ? LeewayRibbonGetHWND(g_ribbon) : NULL;
}

void ribbon_on_idle_state(void)
{
    if (g_ribbon)
        LeewayRibbonInvalidateAll(g_ribbon);
}

BOOL ribbon_on_notify(NMHDR *hdr)
{
    if (!g_ribbon || hdr->hwndFrom != LeewayRibbonGetHWND(g_ribbon))
        return FALSE;

    if (hdr->code == LWUI_RBN_HEIGHTCHANGED || hdr->code == LWUI_RBN_MINIMIZED)
    {
        RECT rc;
        GetClientRect(g_mainWnd, &rc);
        SendMessageW(g_mainWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
        return TRUE;
    }
    if (hdr->code == LWUI_RBN_QATCHANGED)
        return TRUE;

    return FALSE;
}

BOOL ribbon_pretranslate(MSG *msg)
{
    return g_ribbon && LeewayRibbonPreTranslateMessage(g_ribbon, msg);
}

void ribbon_set_preview(BOOL active)
{
    if (g_ribbon)
        LeewayRibbonSetContext(g_ribbon, L"PrintPreview", active);
}

void ribbon_set_dpi(UINT dpi)
{
    if (g_ribbon)
        LeewayRibbonSetDPI(g_ribbon, dpi);
}

void ribbon_on_theme_changed(void)
{
    if (g_ribbon)
        LeewayRibbonOnThemeChanged(g_ribbon);
}
