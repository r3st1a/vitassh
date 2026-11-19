#include "ime.h"

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>

#include <string.h>

#include "log.h"

/* Long enough for a pasted command line; the IME itself caps at 2048. */
#define IME_BUFFER_CHARS 512
#define IME_UTF8_CAP (IME_BUFFER_CHARS * 4 + 1)

static SceWChar16 ime_buffer[IME_BUFFER_CHARS + 1];
static SceWChar16 ime_initial[IME_BUFFER_CHARS + 1];
static SceWChar16 ime_title[128];
static char ime_utf8[IME_UTF8_CAP];

static bool ime_open_;
static ImeStatus ime_state = IME_IDLE;

/* --- UTF-16 helpers ----------------------------------------------------- */

static void ascii_to_utf16(SceWChar16 *dst, size_t cap, const char *src)
{
	size_t i;

	if (cap == 0) return;

	for (i = 0; i + 1 < cap && src[i] != '\0'; i++)
		dst[i] = (SceWChar16)(unsigned char)src[i];
	dst[i] = 0;
}

static size_t utf8_put(char *dst, size_t o, size_t cap, uint32_t cp)
{
	if (cp < 0x80) {
		if (o + 1 >= cap) return o;
		dst[o++] = (char)cp;
	} else if (cp < 0x800) {
		if (o + 2 >= cap) return o;
		dst[o++] = (char)(0xC0 | (cp >> 6));
		dst[o++] = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		if (o + 3 >= cap) return o;
		dst[o++] = (char)(0xE0 | (cp >> 12));
		dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		dst[o++] = (char)(0x80 | (cp & 0x3F));
	} else {
		if (o + 4 >= cap) return o;
		dst[o++] = (char)(0xF0 | (cp >> 18));
		dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
		dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		dst[o++] = (char)(0x80 | (cp & 0x3F));
	}
	return o;
}

static void utf16_to_utf8(const SceWChar16 *src, char *dst, size_t cap)
{
	size_t i;
	size_t o = 0;

	if (cap == 0) return;

	for (i = 0; src[i] != 0; i++) {
		uint32_t cp = src[i];

		/* Rejoin a surrogate pair so emoji and rare CJK survive the trip. */
		if (cp >= 0xD800 && cp <= 0xDBFF && src[i + 1] >= 0xDC00 &&
		    src[i + 1] <= 0xDFFF) {
			cp = 0x10000u + ((cp - 0xD800u) << 10) + (src[i + 1] - 0xDC00u);
			i++;
		}

		o = utf8_put(dst, o, cap, cp);
		if (o + 4 >= cap) break;
	}

	dst[o] = '\0';
}

/* --- setup -------------------------------------------------------------- */

int ime_init(void)
{
	sceAppUtilInit(&(SceAppUtilInitParam){0}, &(SceAppUtilBootParam){0});
	sceCommonDialogSetConfigParam(&(SceCommonDialogConfigParam){0});

	return 0;
}

int ime_open(const char *title, const char *initial, int max_len, ImeKind kind,
             bool password)
{
	SceImeDialogParam param;
	int rc;

	if (ime_open_) return -1;

	if (max_len > IME_BUFFER_CHARS) max_len = IME_BUFFER_CHARS;
	if (max_len < 1) max_len = 1;

	memset(ime_buffer, 0, sizeof(ime_buffer));
	ascii_to_utf16(ime_title, sizeof(ime_title) / sizeof(ime_title[0]),
	               title != NULL ? title : "");
	ascii_to_utf16(ime_initial, sizeof(ime_initial) / sizeof(ime_initial[0]),
	               initial != NULL ? initial : "");

	sceImeDialogParamInit(&param);
	param.inputMethod = SCE_IME_TYPE_DEFAULT;
	param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH |
	                           SCE_IME_LANGUAGE_SIMPLIFIED_CHINESE |
	                           SCE_IME_LANGUAGE_TRADITIONAL_CHINESE |
	                           SCE_IME_LANGUAGE_JAPANESE;
	param.languagesForced = SCE_TRUE;
	param.type = (kind == IME_KIND_NUMBER) ? SCE_IME_TYPE_NUMBER
	                                       : SCE_IME_TYPE_DEFAULT;

	/*
	 * Auto-capitalisation helps nowhere in this app -- it would turn a shell
	 * command or a user name into something else. Candidates are wanted for
	 * commands (that is how Chinese gets typed) but not for a host name or a
	 * password, where they only get in the way.
	 */
	param.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
	if (kind == IME_KIND_HOST || password)
		param.option |= SCE_IME_OPTION_NO_ASSISTANCE;

	param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
	param.textBoxMode = password ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD
	                             : SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
	param.title = ime_title;
	param.maxTextLength = (SceUInt32)max_len;
	param.initialText = ime_initial;
	param.inputTextBuffer = ime_buffer;
	/* SceImeEnterLabel, not SceImeDialogButton: they are different enums and
	 * 2 would be SCE_IME_ENTER_LABEL_SEARCH, i.e. a button labelled "Search". */
	param.enterLabel = SCE_IME_ENTER_LABEL_DEFAULT;

	rc = sceImeDialogInit(&param);
	if (rc < 0) {
		log_printf("ime: sceImeDialogInit = 0x%08X", rc);
		return -1;
	}

	ime_open_ = true;
	ime_state = IME_ACTIVE;
	return 0;
}

ImeStatus ime_poll(void)
{
	if (!ime_open_) return ime_state;

	if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
		ime_state = IME_ACTIVE;
		return ime_state;
	}

	{
		SceImeDialogResult result;

		memset(&result, 0, sizeof(result));
		sceImeDialogGetResult(&result);

		/* The dialog reports Enter versus Close; that is all we need, and it
		 * avoids depending on the result-code enum. */
		if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
			utf16_to_utf8(ime_buffer, ime_utf8, sizeof(ime_utf8));
			ime_state = IME_ACCEPTED;
		} else {
			ime_utf8[0] = '\0';
			ime_state = IME_CANCELLED;
		}
	}

	log_printf("ime: closed, %u bytes", (unsigned)strlen(ime_utf8));

	sceImeDialogTerm();
	ime_open_ = false;
	return ime_state;
}

const char *ime_text(void)
{
	return ime_utf8;
}
