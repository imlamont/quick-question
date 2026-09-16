#include "buf.h"
#include "config.h"
#include "log.h"
#include "openai.h"
#include "prompt.h"
#include "qq.h"
#include "tools.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char usage[] =
	"usage: qq [-hclrvwx] [-d profile] [-p profile] [-m model] [-s text] [-t secs]\n"
	"          [-L file] [--] prompt...\n";

static const char help[] =
	"\n"
	"Ask an AI model a quick question and print the answer as plain text.\n"
	"\n"
	"options:\n"
	"  -h          show this help\n"
	"  -v          show version\n"
	"  -l          list profile names, marking the one that would be used\n"
	"  -c          include shell context (OS, shell, cwd) in the prompt\n"
	"  -r          let the model read files (read_file, list_directory, search_files)\n"
	"  -w          let the model create and edit files (write_file, edit_file)\n"
	"  -x          let the model run shell commands (run_command)\n"
	"  -p profile  use profile for this call\n"
	"  -d profile  save profile as the default, then run the prompt if one is given\n"
	"  -m model    override the profile's model\n"
	"  -s text     append extra steering to the system prompt\n"
	"  -t secs     timeout (default: config \"timeout\", else 120)\n"
	"  -L file     append a timestamped log of the whole exchange to file\n"
	"\n"
	"-r, -w and -x combine (-rw, -xw, ...) and imply -c. qq asks on the terminal\n"
	"before every write, edit, command, and read outside the current directory.\n"
	"\n"
	"Piped stdin is appended to the prompt:  git diff | qq summarize this\n"
	"config: $QQ_CONFIG, $XDG_CONFIG_HOME/qq/config.json or ~/.config/qq/config.json\n";

static int parse_timeout(const char *s, long *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end || v < 1 || v > QQ_TIMEOUT_MAX)
		return -1;
	*out = v;
	return 0;
}

int main(int argc, char **argv)
{
	const char *opt_profile = NULL, *opt_default = NULL, *opt_model = NULL, *opt_system = NULL;
	const char *opt_log = NULL;
	const char *parts[6];
	long opt_timeout = 0, timeout;
	int opt_context = 0, opt_tools = 0, opt_list = 0, ch, r, rc = 2;
	char err[1024] = "", *path = NULL, *reply = NULL, *text;
	struct buf prompt = {0}, input = {0}, context = {0}, tools = {0}, sys = {0}, user = {0};
	struct buf list = {0};
	struct config cfg = {0};
	struct profile prof;

	/* '+' stops at the first non-option, so "qq how do I ls -la" keeps -la. */
	while ((ch = getopt(argc, argv, "+hlvcrwxd:p:m:s:t:L:")) != -1) {
		switch (ch) {
		case 'h':
			fputs(usage, stdout);
			fputs(help, stdout);
			return 0;
		case 'v':
			puts("qq " QQ_VERSION);
			return 0;
		case 'l':
			opt_list = 1;
			break;
		case 'c':
			opt_context = 1;
			break;
		case 'r':
		case 'w':
		case 'x':
			/* Tools need to know where they are, so they imply -c. */
			opt_tools |= ch == 'r' ? TOOLS_READ : ch == 'w' ? TOOLS_WRITE : TOOLS_EXEC;
			opt_context = 1;
			break;
		case 'd':
			opt_default = optarg;
			break;
		case 'p':
			opt_profile = optarg;
			break;
		case 'm':
			opt_model = optarg;
			break;
		case 's':
			opt_system = optarg;
			break;
		case 'L':
			opt_log = optarg;
			break;
		case 't':
			if (parse_timeout(optarg, &opt_timeout)) {
				fprintf(stderr, "qq: invalid timeout: %s\n", optarg);
				return 2;
			}
			break;
		default:
			fputs(usage, stderr);
			return 2;
		}
	}
	for (int i = optind; i < argc; i++) {
		if (i > optind)
			buf_puts(&prompt, " ");
		buf_puts(&prompt, argv[i]);
	}
	if (!prompt.len && !opt_default && !opt_list && isatty(STDIN_FILENO)) {
		fputs(usage, stderr);
		goto out;
	}

	/* Opened before anything else can fail, so the log explains that too. */
	if (opt_log && log_open(opt_log, err, sizeof err))
		goto fail;
	log_command(argc, argv);
	log_printf("start qq " QQ_VERSION " pid=%ld", (long)getpid());

	if (!(path = config_path())) {
		snprintf(err, sizeof err, "cannot locate config: set $QQ_CONFIG or $HOME");
		goto fail;
	}
	if (config_load(&cfg, path, err, sizeof err))
		goto fail;
	if (opt_default) {
		if ((r = config_set_default(&cfg, path, opt_default, err, sizeof err))) {
			rc = r == -2 ? 1 : 2;
			goto fail;
		}
		fprintf(stderr, "qq: default profile set to %s\n", opt_default);
		if (!prompt.len && !opt_list) {
			rc = 0;
			goto out;
		}
	}
	/* Listing is a query, like -h: it reports and exits, prompt or not. */
	if (opt_list) {
		config_list(&cfg, opt_profile, &list);
		if (list.data && (fputs(list.data, stdout) == EOF || fflush(stdout) == EOF)) {
			rc = 1;
			snprintf(err, sizeof err, "write error: %s", strerror(errno));
			goto fail;
		}
		rc = 0;
		goto out;
	}

	if (config_profile(&cfg, opt_profile, &prof, err, sizeof err))
		goto fail;
	if (opt_model && *opt_model)
		prof.model = opt_model;
	log_printf("profile %s model=%s endpoint=%s tools=%s%s%s", prof.name, prof.model,
		   prof.endpoint, opt_tools & TOOLS_READ ? "r" : "",
		   opt_tools & TOOLS_WRITE ? "w" : "", opt_tools & TOOLS_EXEC ? "x" : "");
	/* Said plainly rather than ignored: the flag was asked for on purpose. */
	if (opt_tools && !prof.tools) {
		snprintf(err, sizeof err,
			 "profile \"%s\" has \"tools\": false, so -r, -w and -x cannot be used with it",
			 prof.name);
		goto fail;
	}

	if (!isatty(STDIN_FILENO)) {
		r = buf_read_fd(&input, STDIN_FILENO, QQ_STDIN_MAX);
		if (r == -2) {
			snprintf(err, sizeof err, "stdin exceeds %d MiB", QQ_STDIN_MAX >> 20);
			goto fail;
		}
		if (r) {
			rc = 1;
			snprintf(err, sizeof err, "cannot read stdin: %s", strerror(errno));
			goto fail;
		}
	}
	if (!prompt.len && !input.len) {
		fputs(usage, stderr);
		goto out;
	}

	if (opt_context)
		prompt_context(&context);
	tools_describe(&tools, opt_tools);
	parts[0] = prompt_steer;
	parts[1] = cfg.system_prompt;
	parts[2] = prof.system_prompt;
	parts[3] = opt_system;
	parts[4] = context.data;
	parts[5] = tools.data;
	prompt_system(&sys, parts, sizeof parts / sizeof *parts);
	prompt_user(&user, prompt.data, input.data, input.len);
	timeout = opt_timeout ? opt_timeout : cfg.timeout;
	log_printf("timeout %lds", timeout);

	rc = 1;
	if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
		snprintf(err, sizeof err, "curl initialization failed");
		goto fail;
	}
	reply = openai_ask(&prof, sys.data, user.data, opt_tools, timeout, err, sizeof err);
	curl_global_cleanup();
	if (!reply)
		goto fail;

	text = prompt_clean(reply);
	if (log_on()) {
		char *shown = log_escape(text, strlen(text));

		log_printf("answer %s", shown);
		free(shown);
	}
	if (!*text) {
		snprintf(err, sizeof err, "empty response");
		goto fail;
	}
	if (puts(text) == EOF || fflush(stdout) == EOF) {
		snprintf(err, sizeof err, "write error: %s", strerror(errno));
		goto fail;
	}
	rc = 0;
	goto out;
fail:
	log_printf("failed %s", err);
	fprintf(stderr, "qq: %s\n", err);
out:
	log_printf("exit %d", rc);
	log_close();
	free(reply);
	free(path);
	config_free(&cfg);
	buf_free(&prompt);
	buf_free(&input);
	buf_free(&context);
	buf_free(&tools);
	buf_free(&sys);
	buf_free(&user);
	buf_free(&list);
	return rc;
}
