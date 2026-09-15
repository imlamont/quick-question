#include "prompt.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

const char prompt_steer[] =
	"You are qq, a command-line assistant. Answer directly and concisely in "
	"plain text: no markdown, no preamble, no follow-up offers. If the answer "
	"is a command, give the command first.";

void prompt_system(struct buf *out, const char *const parts[], size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (!parts[i] || !*parts[i])
			continue;
		if (out->len)
			buf_puts(out, "\n\n");
		buf_puts(out, parts[i]);
	}
}

void prompt_context(struct buf *out)
{
	struct utsname u;
	char cwd[PATH_MAX];
	const char *shell = getenv("SHELL"), *slash;
	int wsl;

	if (uname(&u)) {
		strcpy(u.sysname, "unknown");
		u.release[0] = '\0';
	}
	wsl = strstr(u.release, "microsoft") || strstr(u.release, "Microsoft");
	if (shell && (slash = strrchr(shell, '/')))
		shell = slash + 1;
	if (!getcwd(cwd, sizeof cwd))
		strcpy(cwd, "unknown");

	buf_appendf(out, "Environment: OS=%s %s%s, shell=%s, cwd=%s", u.sysname, u.release,
		    wsl ? " (WSL)" : "", shell && *shell ? shell : "unknown", cwd);
}

void prompt_user(struct buf *out, const char *prompt, const char *input, size_t input_len)
{
	if (prompt && *prompt)
		buf_puts(out, prompt);
	if (!input_len)
		return;
	if (!out->len) {
		buf_append(out, input, input_len);
		return;
	}
	buf_puts(out, "\n\n<stdin>\n");
	buf_append(out, input, input_len);
	if (input[input_len - 1] != '\n')
		buf_puts(out, "\n");
	buf_puts(out, "</stdin>");
}

char *prompt_clean(char *reply)
{
	char *s = str_trim(reply), *end;

	if (!strncmp(s, "<think>", strlen("<think>")) && (end = strstr(s, "</think>")))
		s = str_trim(end + strlen("</think>"));
	return s;
}
