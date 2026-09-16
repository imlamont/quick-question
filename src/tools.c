#include "tools.h"
#include "proc.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define READ_MAX (256 * 1024)          /* bytes of a file returned by read_file */
#define EDIT_MAX (4 * 1024 * 1024)     /* largest file edit_file will change */
#define LIST_MAX 1000                  /* entries shown by list_directory */
#define SEARCH_MAX 200                 /* matching lines returned by search_files */
#define SEARCH_FILE_MAX (1024 * 1024)  /* larger files are skipped */
#define SEARCH_DEPTH 16
#define OUTPUT_MAX (32 * 1024)         /* per output stream of run_command */
#define PREVIEW_MAX 600                /* bytes of content shown when asking */

static const struct {
	const char *name;
	int flag;
} tool_table[] = {
	{ "read_file", TOOLS_READ },
	{ "list_directory", TOOLS_READ },
	{ "search_files", TOOLS_READ },
	{ "write_file", TOOLS_WRITE },
	{ "edit_file", TOOLS_WRITE },
	{ "run_command", TOOLS_EXEC },
};

int tools_flag(const char *name)
{
	for (size_t i = 0; i < sizeof tool_table / sizeof *tool_table; i++)
		if (!strcmp(name, tool_table[i].name))
			return tool_table[i].flag;
	return 0;
}

static char flag_letter(int flag)
{
	return flag == TOOLS_READ ? 'r' : flag == TOOLS_WRITE ? 'w' : 'x';
}

/* Append an OpenAI function tool; properties is a JSON object literal and
 * required a JSON array literal. */
static void add_function(cJSON *tools, const char *name, const char *description,
			 const char *properties, const char *required)
{
	cJSON *tool = cJSON_CreateObject(), *fn, *params;

	cJSON_AddStringToObject(tool, "type", "function");
	fn = cJSON_AddObjectToObject(tool, "function");
	cJSON_AddStringToObject(fn, "name", name);
	cJSON_AddStringToObject(fn, "description", description);
	params = cJSON_AddObjectToObject(fn, "parameters");
	cJSON_AddStringToObject(params, "type", "object");
	cJSON_AddItemToObject(params, "properties", cJSON_Parse(properties));
	cJSON_AddItemToObject(params, "required", cJSON_Parse(required));
	cJSON_AddItemToArray(tools, tool);
}

void tools_add_defs(cJSON *req, int enabled)
{
	cJSON *tools;

	if (!enabled)
		return;
	if (!(tools = cJSON_GetObjectItemCaseSensitive(req, "tools")))
		tools = cJSON_AddArrayToObject(req, "tools");

	if (enabled & TOOLS_READ) {
		add_function(tools, "read_file",
			     "Read a text file (up to 256 KB). Paths are relative to the current directory.",
			     "{\"path\":{\"type\":\"string\"}}", "[\"path\"]");
		add_function(tools, "list_directory",
			     "List the entries of a directory; subdirectories end with /.",
			     "{\"path\":{\"type\":\"string\",\"description\":\"defaults to .\"}}", "[]");
		add_function(tools, "search_files",
			     "Find lines containing pattern (plain text, case sensitive) in the files "
			     "under a directory, skipping hidden files. Returns path:line: text, "
			     "up to 200 matches.",
			     "{\"pattern\":{\"type\":\"string\"},"
			     "\"path\":{\"type\":\"string\",\"description\":\"file or directory, defaults to .\"}}",
			     "[\"pattern\"]");
	}
	if (enabled & TOOLS_WRITE) {
		add_function(tools, "write_file",
			     "Create or overwrite a file with content. The user approves each write.",
			     "{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}}",
			     "[\"path\",\"content\"]");
		add_function(tools, "edit_file",
			     "Replace old_text, which must occur exactly once in the file, with new_text. "
			     "The user approves each edit.",
			     "{\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},"
			     "\"new_text\":{\"type\":\"string\"}}",
			     "[\"path\",\"old_text\",\"new_text\"]");
	}
	if (enabled & TOOLS_EXEC)
		add_function(tools, "run_command",
			     "Run a shell command with /bin/sh -c in the current directory and return "
			     "its exit status and output. The user approves each command.",
			     "{\"command\":{\"type\":\"string\"}}", "[\"command\"]");
}

void tools_describe(struct buf *out, int enabled)
{
	if (!enabled)
		return;
	buf_puts(out, "You can use tools on the user's computer. Paths are relative to the "
		      "current directory.");
	if (enabled & (TOOLS_WRITE | TOOLS_EXEC))
		buf_puts(out, " The user approves each file change and command, and any read "
			      "outside the current directory");
	else
		buf_puts(out, " The user approves any read outside the current directory");
	buf_puts(out, "; if they deny one, don't retry it. Use tools only when the question "
		      "needs them, then answer in plain text.");
}

char *tools_replace_once(const char *text, const char *old_text, const char *new_text,
			 size_t *count)
{
	size_t old_len = strlen(old_text);
	const char *hit = NULL, *p = text;
	struct buf b = {0};

	*count = 0;
	if (!old_len)
		return NULL;
	while ((p = strstr(p, old_text))) {
		if (!hit)
			hit = p;
		(*count)++;
		p += old_len;
	}
	if (*count != 1)
		return NULL;
	buf_append(&b, text, (size_t)(hit - text));
	buf_puts(&b, new_text);
	buf_puts(&b, hit + old_len);
	return buf_steal(&b);
}

/* Canonical path of path, or of its parent directory plus its last component
 * when it doesn't exist yet. malloc'd, or NULL. */
static char *resolve(const char *path)
{
	char *real = realpath(path, NULL), *dir_copy, *base_copy, *parent = NULL;
	struct buf b = {0};

	if (real || errno != ENOENT)
		return real;
	dir_copy = strdup(path);
	base_copy = strdup(path);
	if (dir_copy && base_copy)
		parent = realpath(dirname(dir_copy), NULL);
	if (parent)
		buf_appendf(&b, "%s/%s", strcmp(parent, "/") ? parent : "", basename(base_copy));
	free(parent);
	free(dir_copy);
	free(base_copy);
	return b.data;
}

int tools_inside_cwd(const char *path)
{
	char *cwd = realpath(".", NULL), *real = resolve(path);
	int inside = 0;

	if (cwd && real) {
		size_t n = strlen(cwd);

		inside = !strcmp(cwd, "/") ||
			 (!strncmp(real, cwd, n) && (real[n] == '/' || real[n] == '\0'));
	}
	free(cwd);
	free(real);
	return inside;
}

/* Append at most max bytes of s for display on a terminal. Control characters
 * become '?' so the model can't send escape sequences; newlines and tabs are
 * kept only when multiline is set. */
static void append_preview(struct buf *b, const char *s, size_t max, int multiline)
{
	size_t n = strlen(s);

	for (size_t i = 0; i < n && i < max; i++) {
		unsigned char c = (unsigned char)s[i];
		char shown = (char)c;

		if (c == 0x7f || (c < 0x20 && !(multiline && (c == '\n' || c == '\t'))))
			shown = '?';
		buf_append(b, &shown, 1);
	}
	if (n > max)
		buf_appendf(b, "... (%zu more bytes)", n - max);
}

/* Log a tool action on stderr so the user can see what ran. */
static void note(const char *name, const char *arg)
{
	struct buf b = {0};

	append_preview(&b, arg, 200, 0);
	fprintf(stderr, "qq: %s %s\n", name, b.data ? b.data : "");
	buf_free(&b);
}

/* Waiting at a prompt doesn't count against -t, so without a limit an
 * unattended run would wait for an answer forever. */
#define APPROVAL_TIMEOUT_MS (120 * 1000)

int tools_read_answer(int fd, long long timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	char answer[64], *a;
	ssize_t n;
	int r;

	while ((r = poll(&pfd, 1, timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms)) < 0)
		if (errno != EINTR)
			return TOOLS_ANSWER_NO;
	if (r == 0)
		return TOOLS_ANSWER_TIMEOUT;

	while ((n = read(fd, answer, sizeof answer - 1)) < 0 && errno == EINTR)
		;
	if (n <= 0) /* end of input counts as no */
		return TOOLS_ANSWER_NO;
	answer[n] = '\0';
	if ((a = strchr(answer, '\n')))
		*a = '\0';
	a = str_trim(answer);
	for (char *p = a; *p; p++)
		*p = (char)tolower((unsigned char)*p);
	return !strcmp(a, "y") || !strcmp(a, "yes") ? TOOLS_ANSWER_YES : TOOLS_ANSWER_NO;
}

/* Ask on the terminal. Returns TOOLS_ANSWER_*, or -1 when there is no terminal
 * to ask. The time spent waiting is added to *waited_ms. */
static int confirm(const char *question, long long *waited_ms)
{
	long long start = proc_now_ms();
	int fd = open("/dev/tty", O_RDWR | O_CLOEXEC), r;

	if (fd < 0)
		return -1;
	dprintf(fd, "\n%s\nAllow? [y/N] ", question);
	r = tools_read_answer(fd, APPROVAL_TIMEOUT_MS);
	if (r == TOOLS_ANSWER_TIMEOUT)
		dprintf(fd, "\nqq: no answer after %d seconds, so this was not done\n",
			APPROVAL_TIMEOUT_MS / 1000);
	close(fd);
	*waited_ms += proc_now_ms() - start;
	return r;
}

/* Ask the question in q (then freed). On refusal, explain why in out and set
 * *refused, which tells the caller not to ask this again. */
static int allowed(struct buf *q, long long *waited_ms, struct buf *out, int *refused)
{
	int r = confirm(q->data, waited_ms);

	buf_free(q);
	if (r == TOOLS_ANSWER_YES)
		return 1;
	*refused = 1;
	if (r == TOOLS_ANSWER_TIMEOUT)
		buf_appendf(out, "error: not done: the user did not answer within %d seconds",
			    APPROVAL_TIMEOUT_MS / 1000);
	else if (r < 0)
		buf_puts(out, "error: not done: there is no terminal to ask the user for approval");
	else
		buf_puts(out, "error: the user denied this; don't retry it");
	return 0;
}

/* Load the regular file at path into data. Returns 0, -2 if it holds more
 * than max bytes, or -1 with an explanation in out. */
static int load(const char *path, size_t max, struct buf *data, struct buf *out)
{
	struct stat st;
	int fd, r, e;

	/* O_NONBLOCK so opening a FIFO can't hang; it's rejected just below. */
	if ((fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)) < 0) {
		buf_appendf(out, "error: cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		buf_appendf(out, "error: %s is not a regular file", path);
		close(fd);
		return -1;
	}
	r = buf_read_fd(data, fd, max);
	e = errno;
	close(fd);
	if (r == -1) {
		buf_appendf(out, "error: cannot read %s: %s", path, strerror(e));
		return -1;
	}
	if (data->len && memchr(data->data, '\0', data->len)) {
		buf_appendf(out, "error: %s looks like a binary file", path);
		return -1;
	}
	return r;
}

static int save(const char *path, const char *content, size_t len, struct buf *out)
{
	FILE *f = fopen(path, "w");
	int failed;

	if (!f) {
		buf_appendf(out, "error: cannot write %s: %s", path, strerror(errno));
		return -1;
	}
	failed = fwrite(content, 1, len, f) != len;
	failed |= fclose(f) != 0;
	if (failed) {
		buf_appendf(out, "error: cannot write %s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

static void read_file(const char *path, struct buf *out)
{
	struct buf data = {0};
	int r = load(path, READ_MAX, &data, out);

	if (r == -1)
		;
	else if (!data.len)
		buf_puts(out, "(empty file)");
	else if (r == -2)
		buf_appendf(out, "%.*s\n[truncated after %d bytes]", READ_MAX, data.data, READ_MAX);
	else
		buf_append(out, data.data, data.len);
	buf_free(&data);
}

static int by_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void list_directory(const char *path, struct buf *out)
{
	DIR *dir = opendir(path);
	struct dirent *e;
	char **names = NULL;
	size_t n = 0, cap = 0, total = 0;

	if (!dir) {
		buf_appendf(out, "error: cannot list %s: %s", path, strerror(errno));
		return;
	}
	while ((e = readdir(dir))) {
		struct buf full = {0}, name = {0};
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || ++total > LIST_MAX)
			continue;
		buf_appendf(&full, "%s/%s", path, e->d_name);
		buf_puts(&name, e->d_name);
		if (!lstat(full.data, &st) && S_ISDIR(st.st_mode))
			buf_puts(&name, "/");
		buf_free(&full);
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			if (!(names = realloc(names, cap * sizeof *names))) {
				fputs("qq: out of memory\n", stderr);
				exit(1);
			}
		}
		names[n++] = buf_steal(&name);
	}
	closedir(dir);

	if (n)
		qsort(names, n, sizeof *names, by_name);
	for (size_t i = 0; i < n; i++) {
		buf_appendf(out, "%s%s", i ? "\n" : "", names[i]);
		free(names[i]);
	}
	free(names);
	if (!n)
		buf_puts(out, "(empty directory)");
	if (total > LIST_MAX)
		buf_appendf(out, "\n[%zu more entries not shown]", total - LIST_MAX);
}

struct search {
	const char *pattern;
	struct buf *out;
	int matches;
};

static void search_file(struct search *s, const char *path)
{
	struct stat st;
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	long lineno = 0;
	FILE *f;

	if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size > SEARCH_FILE_MAX ||
	    !(f = fopen(path, "r")))
		return;
	while (s->matches < SEARCH_MAX && (len = getline(&line, &cap, f)) > 0) {
		lineno++;
		if (memchr(line, '\0', (size_t)len)) /* binary file */
			break;
		if (strstr(line, s->pattern)) {
			if (line[len - 1] == '\n')
				line[len - 1] = '\0';
			buf_appendf(s->out, "%s:%ld: %.200s\n", path, lineno, line);
			s->matches++;
		}
	}
	free(line);
	fclose(f);
}

static void search_dir(struct search *s, const char *dir, int depth)
{
	struct dirent *e;
	DIR *d;

	if (depth > SEARCH_DEPTH || !(d = opendir(dir)))
		return;
	while (s->matches < SEARCH_MAX && (e = readdir(d))) {
		struct buf path = {0};
		struct stat st;

		if (e->d_name[0] == '.') /* hidden entries, "." and ".." */
			continue;
		if (strcmp(dir, "."))
			buf_appendf(&path, "%s/", dir);
		buf_puts(&path, e->d_name);
		if (!lstat(path.data, &st)) {
			if (S_ISDIR(st.st_mode))
				search_dir(s, path.data, depth + 1);
			else if (S_ISREG(st.st_mode))
				search_file(s, path.data);
		}
		buf_free(&path);
	}
	closedir(d);
}

static void search_files(const char *path, const char *pattern, struct buf *out)
{
	struct search s = { pattern, out, 0 };
	struct stat st;

	if (stat(path, &st)) {
		buf_appendf(out, "error: cannot search %s: %s", path, strerror(errno));
		return;
	}
	if (S_ISDIR(st.st_mode))
		search_dir(&s, path, 0);
	else
		search_file(&s, path);
	if (!s.matches)
		buf_puts(out, "no matches");
	else if (s.matches >= SEARCH_MAX)
		buf_appendf(out, "[stopped after %d matches]", SEARCH_MAX);
}

static void run_command(const char *command, long long deadline_ms, struct buf *out)
{
	const char *const argv[] = { "/bin/sh", "-c", command, NULL };
	struct proc p;
	char err[256];
	int r = proc_run(argv, NULL, 0, OUTPUT_MAX, deadline_ms, &p, err, sizeof err);

	if (r == PROC_ERROR)
		buf_appendf(out, "error: %s", err);
	else if (r == PROC_TIMEOUT)
		buf_puts(out, "the command timed out and was killed");
	else if (WIFEXITED(p.status))
		buf_appendf(out, "exit status %d", WEXITSTATUS(p.status));
	else
		buf_appendf(out, "killed by signal %d", WTERMSIG(p.status));
	if (r != PROC_ERROR) {
		if (p.out.len)
			buf_appendf(out, "\n--- stdout ---\n%s", p.out.data);
		if (p.err.len)
			buf_appendf(out, "\n--- stderr ---\n%s", p.err.data);
		if (p.truncated)
			buf_appendf(out, "\n[output truncated to %d bytes per stream]", OUTPUT_MAX);
	}
	proc_free(&p);
}

static const char *arg_str(const cJSON *args, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);

	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void write_file(const char *path, const cJSON *args, long long *waited_ms,
		       struct buf *out, int *refused)
{
	const char *content = arg_str(args, "content");
	struct buf q = {0};
	struct stat st;

	if (!content) {
		buf_puts(out, "error: missing \"content\" argument");
		return;
	}
	buf_puts(&q, "qq: the model wants to write ");
	append_preview(&q, path, 300, 0);
	buf_appendf(&q, " (%zu bytes%s):\n", strlen(content),
		    stat(path, &st) ? "" : ", replacing the existing file");
	append_preview(&q, content, PREVIEW_MAX, 1);
	if (!allowed(&q, waited_ms, out, refused))
		return;
	note("write_file", path);
	if (!save(path, content, strlen(content), out))
		buf_appendf(out, "wrote %zu bytes to %s", strlen(content), path);
}

static void edit_file(const char *path, const cJSON *args, long long *waited_ms,
		      struct buf *out, int *refused)
{
	const char *old_text = arg_str(args, "old_text"), *new_text = arg_str(args, "new_text");
	struct buf data = {0}, q = {0};
	char *edited = NULL;
	size_t count;
	int r;

	if (!old_text || !new_text) {
		buf_puts(out, "error: missing \"old_text\" or \"new_text\" argument");
		return;
	}
	if ((r = load(path, EDIT_MAX, &data, out))) {
		if (r == -2)
			buf_appendf(out, "error: %s is too large to edit", path);
		goto done;
	}
	/* Check the edit applies before bothering the user. */
	if (!(edited = tools_replace_once(data.data ? data.data : "", old_text, new_text, &count))) {
		buf_appendf(out, "error: old_text occurs %zu times in %s; it must occur exactly once",
			    count, path);
		goto done;
	}
	buf_puts(&q, "qq: the model wants to edit ");
	append_preview(&q, path, 300, 0);
	buf_puts(&q, ", replacing:\n");
	append_preview(&q, old_text, PREVIEW_MAX, 1);
	buf_puts(&q, "\nwith:\n");
	append_preview(&q, new_text, PREVIEW_MAX, 1);
	if (!allowed(&q, waited_ms, out, refused))
		goto done;
	note("edit_file", path);
	if (!save(path, edited, strlen(edited), out))
		buf_appendf(out, "edited %s", path);
done:
	free(edited);
	buf_free(&data);
}

char *tools_call(const cJSON *tool_call, int enabled, long long deadline_ms,
		 long long *waited_ms, int *refused)
{
	const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tool_call, "function");
	const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(fn, "name");
	const cJSON *raw = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
	const char *name = cJSON_IsString(name_item) ? name_item->valuestring : "";
	const char *path, *text;
	struct buf out = {0}, q = {0};
	int flag = tools_flag(name);
	cJSON *args;

	*refused = 0;
	if (!flag) {
		buf_appendf(&out, "error: unknown tool \"%s\"", name);
		return buf_steal(&out);
	}
	if (!(enabled & flag)) {
		buf_appendf(&out, "error: %s is not enabled; the user can allow it by running qq with -%c",
			    name, flag_letter(flag));
		return buf_steal(&out);
	}

	/* Arguments normally arrive JSON-encoded in a string; "" means none. */
	if (cJSON_IsString(raw))
		args = *raw->valuestring ? cJSON_Parse(raw->valuestring) : cJSON_CreateObject();
	else
		args = cJSON_Duplicate(raw, 1);
	if (!cJSON_IsObject(args)) {
		cJSON_Delete(args);
		buf_puts(&out, "error: tool arguments are not a JSON object");
		return buf_steal(&out);
	}

	if (!strcmp(name, "run_command")) {
		if (!(text = arg_str(args, "command"))) {
			buf_puts(&out, "error: missing \"command\" argument");
		} else {
			buf_puts(&q, "qq: the model wants to run a command:\n$ ");
			append_preview(&q, text, PREVIEW_MAX, 1);
			if (allowed(&q, waited_ms, &out, refused)) {
				note("run_command", text);
				run_command(text, deadline_ms + *waited_ms, &out);
			}
		}
		goto done;
	}

	path = arg_str(args, "path");
	if (!path && (!strcmp(name, "list_directory") || !strcmp(name, "search_files")))
		path = ".";
	if (!path) {
		buf_puts(&out, "error: missing \"path\" argument");
	} else if (!strcmp(name, "write_file")) {
		write_file(path, args, waited_ms, &out, refused);
	} else if (!strcmp(name, "edit_file")) {
		edit_file(path, args, waited_ms, &out, refused);
	} else {
		text = arg_str(args, "pattern");
		if (!strcmp(name, "search_files") && (!text || !*text)) {
			buf_puts(&out, "error: missing \"pattern\" argument");
			goto done;
		}
		if (!tools_inside_cwd(path)) {
			buf_appendf(&q, "qq: the model wants to use %s outside the current directory:\n",
				    name);
			append_preview(&q, path, 300, 0);
			if (!allowed(&q, waited_ms, &out, refused))
				goto done;
		}
		note(name, path);
		if (!strcmp(name, "read_file"))
			read_file(path, &out);
		else if (!strcmp(name, "list_directory"))
			list_directory(path, &out);
		else
			search_files(path, text, &out);
	}
done:
	cJSON_Delete(args);
	buf_free(&q);
	return buf_steal(&out);
}
