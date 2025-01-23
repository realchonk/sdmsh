#if __linux__
# define _GNU_SOURCE
# define _XOPEN_SOURCE 700
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <assert.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>
#include <err.h>
#include "make.h"

#define new(T) ((T *)calloc (1, sizeof (T)))
#define MAKEFILE "MyMakefile"
#define SHELL "sh"

static struct macro m_shell = {
	.next = NULL,
	.enext = NULL,
	.prepend = NULL,
	.name = "SHELL",
	.value = SHELL,
	.lazy = 0,
}, m_make = {
	.next = &m_shell,
	.enext = &m_shell,
	.prepend = NULL,
	.name = "MAKE",
	.value = NULL,
	.lazy = 0,
}, m_dmake = {
	.next = &m_make,
	.enext = &m_make,
	.prepend = NULL,
	.name = ".MAKE",
	.value = NULL,
	.lazy = 0,
}, m_makeflags = {
	.next = &m_dmake,
	.enext = &m_dmake,
	.prepend = NULL,
	.name = "MAKEFLAGS",
	.value = NULL,
	.lazy = 0,
}, m_dmakeflags = {
	.next = &m_makeflags,
	.enext = &m_makeflags,
	.prepend = NULL,
	.name = ".MAKEFLAGS",
	.value = NULL,
	.lazy = 0,
};

static struct macro *globals = &m_dmakeflags;
static int verbose = 0;

/* STRING BUFFER */

typedef struct string {
	char *ptr;
	size_t len, cap;
} str_t;

str_new (s)
str_t *s;
{
	s->len = 0;
	s->cap = 10;
	s->ptr = malloc (s->cap + 1);
	return 0;
}

str_reserve (s, n)
str_t *s;
size_t n;
{
	if (s->cap == 0) {
		s->cap = n;
		s->ptr = malloc (s->cap + 1);
	} else if ((s->len + n) > s->cap) {
		for (s->cap *= 2; (s->len + n) > s->cap; s->cap *= 2);
		s->ptr = realloc (s->ptr, s->cap + 1);
	}
	return 0;
}

str_free (s)
str_t *s;
{
	free (s->ptr);
	memset (s, 0, sizeof (*s));
	return 0;
}

str_putc (s, ch)
str_t *s;
{
	str_reserve (s, 1);
	s->ptr[s->len++] = ch;
	return 0;
}

str_write (s, t, n)
str_t *s;
char *t;
size_t n;
{
	str_reserve (s, n);
	memcpy (s->ptr + s->len, t, n);
	s->len += n;
	return 0;
}

str_puts (s, t)
str_t *s;
char *t;
{
	return str_write (s, t, strlen (t));
}

str_last (s)
str_t *s;
{
	return s->len > 0 ? s->ptr[s->len - 1] : EOF;
}

str_pop (s)
str_t *s;
{
	return s->len > 0 ? s->ptr[--s->len] : EOF;
}

str_chomp (s)
str_t *s;
{
	while (str_last (s) == '\n')
		str_pop (s);
	return 0;
}

str_trim (s)
str_t *s;
{
	size_t i;

	while (isspace (str_last (s)))
		str_pop (s);

	for (i = 0; i < s->len && isspace (s->ptr[i]); ++i);

	memmove (s->ptr, s->ptr + i, s->len - i);
	s->len -= i;
	return 0;
}

str_reset (s)
str_t *s;
{
	if (s->cap == 0) {
		str_new (s);
	} else {
		s->len = 0;
	}
	return 0;
}

char *
str_get (s)
str_t *s;
{
	s->ptr[s->len] = '\0';
	return s->ptr;
}

char *
str_release (s)
str_t *s;
{
	char *t;
	s->ptr[s->len] = '\0';
	t = realloc (s->ptr, s->len + 1);
	memset (s, 0, sizeof (*s));
	return t;
}

/* STRING MISC */

char *
xstrcat (s, t)
char *s, *t;
{
	char *u;
	size_t len_s, len_t;

	len_s = strlen (s);
	len_t = strlen (t);
	u = malloc (len_s + len_t + 1);
	memcpy (u, s, len_s);
	memcpy (u + len_s, t, len_t + 1);
	return u;
}

skip_ws (s)
char **s;
{
	for (; isspace (**s); ++*s);
	return 0;
}

char *
ltrim (s)
char *s;
{
	skip_ws (&s);
	return s;
}

char *
rtrim (s)
char *s;
{
	size_t i, len;

	len = strlen (s);
	for (i = len; i > 0 && isspace (s[i - 1]); --i)
		s[i - 1] = '\0';

	return s;
}

char *
trim (s)
char *s;
{
	return ltrim (rtrim (s));
}

starts_with (s, prefix)
char *s, *prefix;
{
	size_t len_s, len_p;

	len_s = strlen (s);
	len_p = strlen (prefix);

	if (len_s < len_p)
		return 0;

	return memcmp (s, prefix, len_p) == 0;
}

/* OTHER MISC */

struct timespec
get_mtime (dir, name)
struct path *dir;
char *name;
{
	extern char *path_cat_str ();
	struct stat st;
	char *path;
	struct timespec t;

	path = path_cat_str (dir, name);

	if (stat (path, &st) == 0) {
		t = st.st_mtim;
	} else {
		memset (&t, 0, sizeof (t));
	}

	return t;
}

struct timespec
now ()
{
	struct timespec t;
	clock_gettime (CLOCK_REALTIME, &t);
	return t;
}

tv_cmp (a, b)
struct timespec *a, *b;
{
	if (a->tv_sec < b->tv_sec) {
		return -1;
	} else if (a->tv_sec > b->tv_sec) {
		return 1;
	} else if (a->tv_nsec < b->tv_nsec) {
		return -1;
	} else if (a->tv_nsec > b->tv_nsec) {
		return 1;
	} else {
		return 0;
	}
}

/* PATH LOGIC */

static struct path path_null = { .type = PATH_NULL, .name = NULL };
static struct path path_super = { .type = PATH_SUPER, .name = NULL };
static struct path tmppath = { .type = PATH_NAME, .name = NULL };

/* return the number of path components (excl. PATH_NULL). */
size_t
path_len (p)
struct path *p;
{
	size_t i;

	for (i = 0; p[i].type != PATH_NULL; ++i);

	return i;
}

struct path *
path_cpy (old, old_len, new_len)
struct path *old;
size_t old_len, new_len;
{
	struct path *p;

	p = calloc (new_len + 1, sizeof (struct path));
	memcpy (p, old, old_len * sizeof (struct path));
	p[new_len].type = PATH_NULL;

	return p;
}

struct path *
path_cat (old, comp)
struct path *old, *comp;
{
	struct path *p;
	size_t len;

	len = path_len (old);

	switch (comp->type) {
	case PATH_NULL:
		p = path_cpy (old, len, len);
		break;
	case PATH_SUPER:
		if (len > 0 && old[len - 1].type != PATH_SUPER) {
			--len;
			p = path_cpy (old, len, len);
			break;
		}
		/* fallthrough */
	case PATH_NAME:
		p = path_cpy (old, len, len + 1);
		p[len] = *comp;
		break;
	}

	return p;
}

path_write (s, p)
str_t *s;
struct path *p;
{
	size_t i;

	str_putc (s, '.');

	for (i = 0; p[i].type != PATH_NULL; ++i) {
		switch (p[i].type) {
		case PATH_NULL:
			abort ();
		case PATH_SUPER:
			str_puts (s, "/..");
			break;
		case PATH_NAME:
			str_putc (s, '/');
			str_puts (s, p[i].name);
			break;
		}
	}

	return 0;
}

static str_t tmpstr;

char *
path_to_str (p)
struct path *p;
{
	str_reset (&tmpstr);
	path_write (&tmpstr, p);
	return str_get (&tmpstr);
}

char *
path_basename (p)
struct path *p;
{
	char *s, *t;

	s = realpath (path_to_str (p), NULL);
	t = strdup (basename (s));
	free (s);

	return t;
}

char *
path_cat_str (dir, file)
struct path *dir;
char *file;
{
	str_reset (&tmpstr);
	path_write (&tmpstr, dir);
	str_putc (&tmpstr, '/');
	str_puts (&tmpstr, file);
	return str_get (&tmpstr);
}

struct path *
parse_path (s)
char *s;
{
	size_t len = 0, cap = 4;
	struct path *p;
	char *t;

	p = calloc (cap + 1, sizeof (struct path));

	while ((t = strsep (&s, "/")) != NULL) {
		if (*t == '\0' || strcmp (t, ".") == 0)
			continue;

		if (len == cap) {
			cap *= 2;
			p = reallocarray (p, cap + 1, sizeof (struct path));
		}

		if (strcmp (t, "..") == 0) {
			p[len].type = PATH_SUPER;
		} else {
			p[len].type = PATH_NAME;
			p[len].name = strdup (t);
		}
		++len;
	}
	p[len].type = PATH_NULL;

	return p;
}

/* MACORS MISC */

/* is macro name */
ismname (ch)
{
	return isalnum (ch) || ch == '_' || ch == '.';
}

/* SEARCHING */

struct macro *
find_emacro (sc, name)
struct scope *sc;
char *name;
{
	struct macro *m;

	if (sc == NULL)
		return NULL;

	for (m = sc->dir->emacros; m != NULL; m = m->enext) {
		if (strcmp (m->name, name) == 0)
			return m;
	}
	
	return find_emacro (sc->parent, name);
}

struct macro *
find_macro (sc, name)
struct scope *sc;
char *name;
{
	struct macro *m;

	for (m = sc->dir->macros; m != NULL; m = m->next) {
		if (strcmp (m->name, name) == 0)
			return m;
	}

	m = find_emacro (sc->parent, name);
	if (m != NULL)
		return m;

	for (m = globals; m != NULL; m = m->next) {
		if (strcmp (m->name, name) == 0)
			return m;
	}

	return NULL;
}

struct template *
find_template (sc, name)
struct scope *sc;
char *name;
{
	struct template *tm;

	for (tm = sc->dir->templates; tm != NULL; tm = tm->next) {
		if (strcmp (tm->name, name) == 0)
			return tm;
	}

	if (sc->parent == NULL)
		errx (1, "template not found: %s", name);

	return find_template (sc->parent, name);
}

struct file *
find_file (dir, name)
struct directory *dir;
char *name;
{
	struct file *f;

	for (f = dir->files; f != NULL; f = f->next) {
		if (strcmp (name, f->name) == 0)
			return f;
	}

	return NULL;
}

/* MACRO EXPANSION */

struct expand_ctx {
	struct file *f;
};

replace_into (out, s, old, new)
str_t *out;
char *s, *old, *new;
{
	size_t len_s, len_old;

	len_s = strlen (s);
	len_old = strlen (old);

	if (len_s < len_old || memcmp (s + len_s - len_old, old, len_old) != 0) {
		str_puts (out, s);
		return 0;
	}

	str_write (out, s, len_s - len_old);
	str_puts (out, new);
	return 0;
}

replace_all_into (out, s, old, new)
str_t *out;
char *s, *old, *new;
{
	char *t;
	int x = 0;

	while ((t = strsep (&s, " \t")) != NULL) {
		if (*t == '\0')
			continue;

		replace_into (out, t, old, new);
		str_putc (out, ' ');
		x = 1;
	}

	if (x)
		str_pop (out);

	return 0;
}

/* ${name}
 * ${name:old_string=new_string}
 * TODO:
 * ${name/suffix}	append `suffix` after each word
 */
subst2 (out, sc, s, ctx)
str_t *out;
struct scope *sc;
char **s;
struct expand_ctx *ctx;
{
	extern char *expand_macro ();
	extern expand_macro_into ();
	extern subst ();
	struct macro *m;
	char *v, *orig = *s;
	str_t name, old, new;

	/* parse macro name */
	str_new (&name);
	while (**s != '\0') {
		if (ismname (**s)) {
			str_putc (&name, **s);
			++*s;
		} else if (**s == '$') {
			++*s;
			subst (&name, sc, s, ctx);
		} else {
			break;
		}
	}

	m = find_macro (sc, str_get (&name));
	str_free (&name);
	if (**s == '}') {
		++*s;
		return expand_macro_into (out, sc, m);
	}

	v = expand_macro (sc, m);

	if (**s != ':') {
	invalid:
		errx (1, "invalid macro expansion: ${%s", orig);
	}
	++*s;

	/* TODO: modifiers */

	str_new (&old);
	str_new (&new);

	while (**s != '\0' && **s != '}' && **s != '=') {
		if (**s == '$') {
			++*s;
			subst (&old, sc, s, ctx);
		} else {
			str_putc (&old, **s);
			++*s;
		}
	}

	if (**s != '=')
		goto invalid;
	++*s;

	while (**s != '\0' && **s != '}') {
		if (**s == '$') {
			++*s;
			subst (&new, sc, s, ctx);
		} else {
			str_putc (&new, **s);
			++*s;
		}
	}

	if (**s != '}')
		goto invalid;
	++*s;

	replace_all_into (out, v, str_get (&old), str_get (&new));

	str_free (&old);
	str_free (&new);
	free (v);
	return 0;
}

subst (out, sc, s, ctx)
str_t *out;
struct scope *sc;
char **s;
struct expand_ctx *ctx;
{
	struct scope *sc2;
	struct dep *dep;
	int ch;

	ch = **s;
	++*s;
	switch (ch) {
	case '$':
		str_putc (out, '$');
		break;
	case '.':
		str_putc (out, '.');
		for (sc2 = sc->parent; sc2 != NULL; sc2 = sc2->parent) {
			str_puts (out, "/..");
		}
		break;
	case '@':
		if (ctx == NULL)
			errx (1, "cannot use $@ here");
		str_puts (out, ctx->f->name);
		break;
	case '<':
		if (ctx == NULL)
			errx (1, "cannot use $< here");

		if (ctx->f->deps == NULL)
			break;

		path_write (out, ctx->f->deps->path);
		break;
	case '*':
		if (ctx == NULL)
			errx (1, "cannot use $< here");
		for (dep = ctx->f->deps; dep != NULL; dep = dep->next) {
			str_putc (out, ' ');
			path_write (out, dep->path);
		}
		break;
	case '{':
		subst2 (out, sc, s, ctx);
		break;
	case '(':
		errx (1, "syntax error: $(...) syntax is reserved for future use, please use ${...} instead.");
	default:
		errx (1, "syntax error: invalid escape sequence: $%c%s", ch, *s);
	}

	return 0;
}

expand_into (out, sc, s, ctx)
str_t *out;
struct scope *sc;
char *s;
struct expand_ctx *ctx;
{
	while (*s != '\0') {
		if (*s != '$') {
			str_putc (out, *s++);
			continue;
		}
		++s;
		subst (out, sc, &s, ctx);
	}
	return 0;
}

expand_macro_into (out, sc, m)
str_t *out;
struct scope *sc;
struct macro *m;
{
	if (m == NULL)
		return -1;

	if (m->prepend != NULL) {
		expand_macro_into (out, sc, m->prepend);
		str_putc (out, ' ');
	}

	if (m->lazy) {
		expand_into (out, sc, m->value, NULL);
	} else {
		str_puts (out, m->value);
	}
	return 0;
}

char *
expand_macro (sc, m)
struct scope *sc;
struct macro *m;
{
	str_t tmp;

	str_new (&tmp);
	expand_macro_into (&tmp, sc, m);
	return str_release (&tmp);
}

char *
expand (sc, s, ctx)
struct scope *sc;
char *s;
struct expand_ctx *ctx;
{
	str_t out;

	str_new (&out);
	expand_into (&out, sc, s, ctx);
	str_trim (&out);
	return str_release (&out);
}

/* COMMAND EXECUTION */

char *
evalcom (sc, dir, cmd)
struct scope *sc;
struct path *dir;
char *cmd;
{
	char *args[] = {
		m_shell.value,
		"-c",
		expand (sc, cmd, NULL),
		NULL,
	};
	ssize_t i, n;
	str_t data;
	pid_t pid;
	int pipefd[2];
	char buf[64 + 1];

	if (pipe (pipefd) != 0)
		err (1, "pipe()");

	pid = fork ();
	if (pid == -1)
		err (1, "fork()");

	if (pid == 0) {
		close (STDOUT_FILENO);
		close (pipefd[0]);
		if (dup (pipefd[1]) != STDOUT_FILENO)
			err (1, "failed to dup");
		close (STDIN_FILENO);
		if (open ("/dev/null", O_RDONLY) != STDIN_FILENO)
			err (1, "failed to open /dev/null");
		close (pipefd[1]);

		if (chdir (path_to_str (dir)) != 0)
			err (1, "failed to chdir");

		execvp (m_shell.value, args);
		err (1, "failed to launch shell");
	} else {
		close (pipefd[1]);

		str_new (&data);

		while ((n = read (pipefd[0], buf, sizeof (buf) - 1)) > 0) {
			for (i = 0; i < n; ++i)
				str_putc (&data, buf[i]);
		}
		close (pipefd[0]);
		wait (NULL);
	}

	free (args[2]);
	str_chomp (&data);

	return str_release (&data);
}

runcom (sc, prefix, cmd, ctx)
struct scope *sc;
struct path *prefix;
char *cmd;
struct expand_ctx *ctx;
{
	str_t fcmd;
	int ec, q = 0;

	if (*cmd == '@') {
		q = 1;
		++cmd;
	}

	str_new (&fcmd);
	str_puts (&fcmd, "cd '");
	path_write (&fcmd, prefix);
	str_puts (&fcmd, "' && ");
	expand_into (&fcmd, sc, cmd, ctx);
	
	if (!q)
		printf ("[%s] $ %s\n", path_to_str (prefix), cmd);

	ec = system (str_get (&fcmd));
	str_free (&fcmd);

	return ec;
}

rungnu (sc, prefix, rule, q)
struct scope *sc;
struct path *prefix;
char *rule;
{
	str_t cmd;
	int ec;

	str_new (&cmd);
	str_puts (&cmd, sc->gnu->prog != NULL ? sc->gnu->prog : "make");

	if (q)
		str_puts (&cmd, " -q");

	if (sc->makefile != NULL) {
		str_puts (&cmd, " -f ");
		str_puts (&cmd, sc->makefile);
	}

	if (rule != NULL) {
		str_putc (&cmd, ' ');
		str_puts (&cmd, rule);
	}

	ec = runcom (sc, prefix, str_get (&cmd), NULL);
	str_free (&cmd);
	return ec;
}

/* EXPRESSION PARSER */

is_truthy (s)
char *s;
{
	char *endp;
	long x;

	if (*s == '\0')
		return 0;

	x = strtol (s, &endp, 0);

	return *endp != '\0' || x != 0;
}

e_command (sc, s, cmd, arg)
struct scope *sc;
char **s, *cmd;
str_t *arg;
{
	char *orig = *s;

	*s += strlen (cmd);
	skip_ws (s);
	if (**s != '(')
		errx (1, "expected '(' after 'defined': %s", orig);

	str_new (arg);
	for (++*s; **s != ')'; ++*s)
		str_putc (arg, **s);
	++*s;

	str_trim (arg);
	return 0;
}

e_atom (sc, s, val)
struct scope *sc;
char **s;
str_t *val;
{
	str_t arg;
	int x;

	skip_ws (s);

	if (**s == '"') {
		++*s;

		while (**s != '"') {
			if (**s == '$') {
				++*s;
				subst (val, sc, s, NULL);
			} else {
				str_putc (val, **s);
				++*s;
			}
		}
		++*s;
	} else if (starts_with (*s, "defined")) {
		e_command (sc, s, "defined", &arg);
		x = find_macro (sc, str_get (&arg)) != NULL;
	comm:
		str_putc (val, x ? '1' : '0');
		str_free (&arg);
	} else if (starts_with (*s, "target")) {
		e_command (sc, s, "target", &arg);
		x = find_file (sc->dir, str_get (&arg)) != NULL;
		goto comm;
	} else {
		errx (1, "invalid expression: '%s'", *s);
	}

	return 0;
}

e_unary (sc, s, val)
struct scope *sc;
char **s;
str_t *val;
{
	skip_ws (s);
	if (**s != '!')
		return e_atom (sc, s, val);
	++*s;

	e_unary (sc, s, val);

	if (is_truthy (str_get (val))) {
		str_reset (val);
		str_putc (val, '0');
	} else {
		str_reset (val);
		str_putc (val, '1');
	}

	return 0;
}

enum {
	COMP_EQ,
	COMP_NE,
};

e_comp (sc, s)
struct scope *sc;
char **s;
{
	str_t left, right;
	char *sl, *sr, *el, *er;
	long il, ir;
	int cmp, x, icmp;

	str_new (&left);
	e_unary (sc, s, &left);

	skip_ws (s);

	if (starts_with (*s, "==")) {
		cmp = COMP_EQ;
		*s += 2;
	} else if (starts_with (*s, "!=")) {
		cmp = COMP_NE;
		*s += 2;
	} else {
		str_trim (&left);
		x = is_truthy (str_get (&left));
		str_free (&left);
		return x;
	}

	skip_ws (s);
	str_new (&right);
	e_unary (sc, s, &right);

	str_trim (&left);
	str_trim (&right);

	sl = str_get (&left);
	sr = str_get (&right);
	il = strtol (sl, &el, 0);
	ir = strtol (sr, &er, 0);
	icmp = *sl != '\0' && *sr != '\0' && *el == '\0' && *er == '\0';
	switch (cmp) {
	case COMP_EQ:
		if (icmp) {
			x = il == ir;
		} else {
			x = strcmp (sl, sr) == 0;
		}
		break;
	case COMP_NE:
		if (icmp) {
			x = il != ir;
		} else {
			x = strcmp (sl, sr) != 0;
		}
		break;
	default:
		abort ();
	}
	
	str_free (&left);
	str_free (&right);
	return x;
}

e_and (sc, s)
struct scope *sc;
char **s;
{
	int x;

	x = e_comp (sc, s);
	while (skip_ws (s), starts_with (*s, "&&")) {
		*s += 2;
		x &= e_comp (sc, s);
	}

	return x;
}

e_or (sc, s)
struct scope *sc;
char **s;
{
	int x;

	x = e_and (sc, s);
	while (skip_ws (s), starts_with (*s, "||")) {
		*s += 2;
		x |= e_and (sc, s);
	}

	return x;
}

parse_expr (sc, s)
struct scope *sc;
char *s;
{
	s = trim (s);
	return e_or (sc, &s);
}

/* PARSER */

char *
readline (file, ln)
FILE *file;
int *ln;
{
	str_t line;
	int ch, eof = 1;

	str_new (&line);

	while (1) {
		ch = fgetc (file);
		switch (ch) {
		case EOF:
			goto ret;
		case '\n':
			eof = 0;
			++*ln;
			goto ret;
		case '\\':
			ch = fgetc (file);
			if (ch == '\n') {
				++*ln;
			} else {
				str_putc (&line, '\\');
				str_putc (&line, ch);
			}
			eof = 0;
			break;
		default:
			str_putc (&line, ch);
			eof = 0;
			break;
		}
	}

ret:
	return eof ? NULL : str_release (&line);
}

/*
 * .include yacc
 * .include yacc, DIR
 * .include libx, GNU
 */
parse_include (sc, dir, s, subdirs)
struct scope *sc;
struct path *dir;
struct macro **subdirs;
char *s;
{
	struct scope *sub;
	struct macro *m;
	char *p, *path;

	sub = new (struct scope);
	sub->next = sc->dir->subdirs;
	sub->parent = sc;
	sub->makefile = NULL;
	sc->dir->subdirs = sub;

	p = strsep (&s, ",");
	if (p == NULL)
		return -1;

	sub->name = strdup (trim (p));

	m = new (struct macro);
	m->next = sc->dir->macros;
	m->enext = NULL;
	m->prepend = *subdirs;
	m->name = ".SUBDIRS";
	m->value = sub->name;
	m->lazy = 0;
	sc->dir->macros = m;
	*subdirs = m;

	path = path_cat_str (dir, sub->name);
	if (access (path, F_OK) != 0)
		errx (1, "%s: directory not found: %s", path_to_str (dir), sub->name);
	
	p = strsep (&s, ",");
	if (p == NULL) {
		sub->type = SC_DIR;
		sub->dir = NULL;
		sub->makefile = MAKEFILE;
		return 0;
	}

	/* TODO: handle empty fields */
	p = trim (p);
	if (strcmp (p, "DIR") == 0) {
		sub->type = SC_DIR;
		sub->dir = NULL;
		sub->makefile = MAKEFILE;

		p = strsep (&s, ",");
		if (p == NULL)
			return 0;

		sub->makefile = strdup (trim (p));
		return 0;
	} else if (strcmp (p, "GNU") == 0) {
		sub->type = SC_GNU;
		sub->gnu = new (struct gnu);
		sub->gnu->prog = NULL;

		p = strsep (&s, ",");
		if (p == NULL)
			return 0;
		sub->gnu->prog = strdup (trim (p));

		p = strsep (&s, ",");
		if (p == NULL)
			return 0;
		sub->makefile = strdup (trim (p));

		return 0;
	} else {
		return -1;
	}
}

struct rule *
parse_rule (sc, dir, s, t, help)
struct scope *sc;
struct path *dir;
char *s, *t, *help;
{
	struct inference *inf;
	struct rule *r;
	struct file *f;
	struct dep *dep, *deps, *dt;
	char *u, *v, *p;
	int flag;

	r = new (struct rule);
	r->code = NULL;
	dt = deps = NULL;

	*t = '\0';

	/* parse deps */
	v = u = expand (sc, t + 1, NULL);
	while ((p = strsep (&v, " \t")) != NULL) {
		if (*p == '\0')
			continue;

		dep = new (struct dep);
		dep->next = NULL;
		dep->path = parse_path (p);

		if (dt != NULL) {
			dt->next = dep;
			dt = dep;
		} else {
			dt = deps = dep;
		}
	}	
	free (u);

	/* parse targets */
	u = expand (sc, s, NULL);
	flag = 1;
	if (u[0] == '.') {
		p = strchr (u + 1, '.');
		inf = new (struct inference);
		inf->next = sc->dir->infs;
		inf->rule = r;
		inf->deps = deps;
		inf->dtail = dt;

		if (p != NULL) {
			*p = '\0';
			inf->from = strdup (u);
			*p = '.';
			inf->to = strdup (p); 
		} else {
			inf->from = strdup (u);
			inf->to = "";
		}

		sc->dir->infs = inf;
	} else {
		v = u;
		while ((p = strsep (&v, " \t")) != NULL) {
			if (*p == '\0')
				continue;
			/* TODO: check name */

			f = find_file (sc->dir, p);
			if (f == NULL) {
				f = new (struct file);
				f->next = sc->dir->files;
				f->name = strdup (p);
				f->rule = r;
				f->deps = deps;
				f->dtail = dt;
				f->mtime = get_mtime (dir, f->name);
				f->help = help;
				sc->dir->files = f;
				continue;
			}

			flag = 0;

			if (f->help == NULL)
				f->help = help;

			if (f->deps == NULL) {
				f->deps = deps;
				f->dtail = dt;
				continue;
			}

			for (dep = f->deps; dep->next != NULL; dep = dep->next);
			dep->next = deps;
			f->dtail = dt;
		}
	}
	free (u);
	return flag ? r : NULL;
}

parse_assign (sc, dir, s, t, help)
struct scope *sc;
struct path *dir;
char *s, *t, *help;
{
	struct macro *m, *m2;

	m = new (struct macro);
	m->next = sc->dir->macros;
	m->enext = NULL;
	m->help = help;
	m->prepend = NULL;

	if (t[-1] == '!') {
		t[-1] = '\0';
		m->lazy = 0;
		m->value = evalcom (sc, dir, trim (t + 1));
	} else if (t[-1] == '?') {
		t[-1] = '\0';
		m2 = find_macro (sc, trim (s));
		if (m2 != NULL) {
			m->value = m2->value;
		} else {
			m->value = strdup (trim (t + 1));
		}
		m->lazy = 1;
	} else if (t[-1] == ':') {
		/* handle both `:=` and `::=` */
		t[t[-2] == ':' ? -2 : -1] = '\0';
		m->lazy = 0;
		m->value = expand (sc, trim (t + 1), NULL);
	} else if (t[-1] == '+') {
		t[-1] = '\0';
		m->value = strdup (trim (t + 1));
		m->prepend = find_macro (sc, trim (s));
		m->lazy = 1;
	} else {
		m->lazy = 1;
		m->value = strdup (trim (t + 1));
	}
	m->name = strdup (trim (s));
	sc->dir->macros = m;
	return 0;
}


#define IF_VAL 0x01
#define IF_HAS 0x02
#define MAX_IFSTACK 16

walkifstack (s, n)
char *s;
size_t n;
{
	size_t i;

	for (i = 0; i < n; ++i) {
		if (!(s[i] & IF_VAL))
			return 0;
	}
	return 1;
}

do_parse (sc, dir, path, file, subdirs)
struct scope *sc;
struct path *dir;
char *path;
FILE *file;
struct macro **subdirs;
{
	extern parse ();
	struct macro *m;
	struct template *tm;
	struct rule *r = NULL;
	size_t len, cap, iflen = 0;
	char *s, *t, *u, *help = NULL;
	char ifstack[MAX_IFSTACK];
	int x, ln = 0, run;
	FILE *tfile;
	str_t text;

	for (; (s = readline (file, &ln)) != NULL; free (s)) {
		run = walkifstack (ifstack, iflen);
		if (s[0] == '#' && s[1] == '#') {
			help = expand (sc, trim (s + 2), NULL);
			continue;
		} else if (s[0] == '#' || *trim (s) == '\0') {
			continue;
		} else if (starts_with (s, "include ")) {
			if (!run)
				goto cont;

			t = expand (sc, s + 8, NULL);
			if (*t == '/') {
				u = t;
			} else {
				u = strdup (path_cat_str (dir, t));
			}
			parse (sc, dir, u);
			if (u != t)
				free (u);
			free (t);
		} else if (starts_with (s, ".include ")) {
			if (!run)
				goto cont;

			if (parse_include (sc, dir, s + 9, subdirs) == -1)
				errx (1, "%s:%d: syntax error", path, ln);
		} else if (starts_with (s, ".export ")) {
			if (!run)
				goto cont;

			t = trim (s + 8);

			for (m = sc->dir->emacros; m != NULL; m = m->enext) {
				if (strcmp (m->name, t) == 0)
					goto cont; /* already exported */
			}

			for (m = sc->dir->macros; m != NULL; m = m->next) {
				if (strcmp (m->name, t) == 0) {
					m->enext = sc->dir->emacros;
					sc->dir->emacros = m;
					goto cont;
				}
			}

			errx (1, "%s:%d: no such macro: %s", path, ln, t);
		} else if (starts_with (s, ".if ")) {
			if (iflen == MAX_IFSTACK)
				errx (1, "%s:%d: maximum .if depth of %d reached", path, ln, MAX_IFSTACK);
			x = parse_expr (sc, trim (s + 4)) & 0x01;
			ifstack[iflen++] = x * (IF_VAL | IF_HAS);
		} else if (starts_with (s, ".else")) {
			if (iflen == 0)
				errx (1, "%s:%d: not in .if", path, ln);
			t = &ifstack[iflen - 1];
			*t = (!(*t & IF_HAS) * (IF_VAL | IF_HAS)) | (*t & IF_HAS);
		} else if (starts_with (s, ".elif ")) {
			if (iflen == 0)
				errx (1, "%s:%d: not in .if", path, ln);
			x = parse_expr (sc, trim (s + 6));
			t = &ifstack[iflen - 1];
			*t = ((!(*t & IF_HAS) && x) * (IF_VAL | IF_HAS)) | (*t & IF_HAS);
		} else if (starts_with (s, ".endif")) {
			if (iflen == 0)
				errx (1, "%s:%d: not in .if", path, ln);
			--iflen;
		} else if (starts_with (s, ".template ")) {
			str_new (&text);

			tm = new (struct template);
			tm->next = sc->dir->templates;
			tm->name = strdup (trim (s + 10));

			for (free (s); (s = readline (file, &ln)) != NULL; free (s)) {
				if (strcmp (s, ".endt") == 0 || strcmp (s, ".endtemplate") == 0)
					break;

				str_puts (&text, s);
				str_putc (&text, '\n');
			}

			if (run) {
				tm->text = str_release (&text);
				sc->dir->templates = tm;
			} else {
				free (tm);
				str_free (&text);
			}
		} else if (starts_with (s, ".expand ")) {
			tm = find_template (sc, trim (s + 8));
			tfile = fmemopen (tm->text, strlen (tm->text), "r");
			do_parse (sc, dir, "template", tfile, subdirs);
			fclose (tfile);
		} else if (starts_with (s, ".DEFAULT:")) {
			sc->dir->default_file = strdup (trim (s + 9));
		} else if (s[0] == '\t') {
			if (!run)
				goto cont;

			if (r == NULL)
				errx (1, "%s:%d: syntax error", path, ln);

			if (len == cap) {
				cap *= 2;
				r->code = reallocarray (r->code, cap + 1, sizeof (char *));
			}

			r->code[len++] = strdup (s + 1);
			r->code[len] = NULL;
		} else if ((t = strchr (s, '=')) != NULL) {
			if (!run)
				goto cont;

			/* TODO: check name */
			*t = '\0';
			parse_assign (sc, dir, s, t, help);
		} else if ((t = strchr (s, ':')) != NULL) {
			if (!run)
				goto cont;

			r = parse_rule (sc, dir, s, t, help);
			if (r == NULL)
				goto cont;

			len = 0;
			cap = 1;
			r->code = calloc (cap + 1, sizeof (char *));
			r->code[0] = NULL;
		} else {
			warnx ("%s:%d: syntax error", path, ln);
		}

	cont:
		help = NULL;
	}

	return 0;
}

parse (sc, dir, path) 
struct scope *sc;
struct path *dir;
char *path;
{
	struct macro *subdirs = NULL;
	FILE *file;

	file = fopen (path, "r");
	if (file == NULL)
		err (1, "open(\"%s\")", path);

	if (sc->dir == NULL) {
		sc->dir = new (struct directory);
		sc->dir->subdirs = NULL;
		sc->dir->files = NULL;
		sc->dir->done = 0;
	} else if (sc->dir->done) {
		errx (1, "%s: parsing this file again?", path);
	}

	do_parse (sc, dir, path, file, &subdirs);
	sc->dir->done = 1;

	fclose (file);
	return 0;
}

parse_dir (sc, dir)
struct scope *sc;
struct path *dir;
{
	char *path;

	path = strdup (path_cat_str (dir, sc->makefile));
	parse (sc, dir, path);
	free (path);

	return 0;
}

struct scope *
parse_recursive (dir, makefile)
struct path *dir;
char *makefile;
{
	struct path *mfpath, *ppath;
	struct scope *sc, *parent;
	char *path, *name;

	tmppath.name = makefile;
	mfpath = path_cat (dir, &tmppath);
	path = path_to_str (mfpath);
	if (access (path, F_OK) != 0) {
		sc = NULL;
		goto ret;
	}

	ppath = path_cat (dir, &path_super);
	parent = parse_recursive (ppath, makefile);
	if (parent == NULL)
		parent = parse_recursive (ppath, MAKEFILE);
	free (ppath);

	name = path_basename (dir);

	if (parent != NULL) {
		if (parent->type != SC_DIR)
			errx (1, "%s: invalid parent type", path_to_str (mfpath));


		for (sc = parent->dir->subdirs; sc != NULL; sc = sc->next) {
			if (strcmp (sc->name, name) == 0) {
				if (sc->type != SC_DIR)
					errx (1, "%s: invalid type", path_to_str (mfpath));
				goto parse;
			}
		}

		goto create;
	} else {
	create:
		sc = new (struct scope);
		sc->type = SC_DIR;
		sc->name = name;
		sc->dir = NULL;
	}

parse:
	sc->makefile = makefile;
	sc->parent = parent;
	path = strdup (path_to_str (mfpath));
	parse (sc, dir, path);
	free (path);

ret:
	free (mfpath);
	return sc;
}

struct path *	
parse_subdir (prefix, sub)
struct path *prefix;
struct scope *sub;
{
	struct path *np;

	tmppath.name = sub->name;
	np = path_cat (prefix, &tmppath);
	parse_dir (sub, np);
	return np;
}

/* INFERENCE RULES */

struct file *
inst_inf (sc, inf, name)
struct scope *sc;
struct inference *inf;
char *name;
{
	struct file *f;
	struct dep *dep;
	char *s, *ext;

	ext = strrchr (name, '.');
	if (ext != NULL)
		*ext = '\0';
	s = xstrcat (name, inf->from);
	if (ext != NULL)
		*ext = '.';

	f = new (struct file);
	f->next = sc->dir->files;
	f->name = strdup (name);
	f->rule = inf->rule;
	f->deps = inf->deps;
	memset (&f->mtime, 0, sizeof (f->mtime));

	dep = new (struct dep);
	dep->next = f->deps;
	dep->path = calloc (2, sizeof (struct path));
	dep->path[0].type = PATH_NAME;
	dep->path[0].name = s;
	dep->path[1].type = PATH_NULL;
	f->deps = dep;
	f->dtail = inf->dtail;
	sc->dir->files = f;

	return f;
}

inf_inst_file (f, inf)
struct file *f;
struct inference *inf;
{
	struct dep *dep;
	char *s, *ext;

	ext = strrchr (f->name, '.');
	if (ext != NULL)
		*ext = '\0';
	s = xstrcat (f->name, inf->from);
	if (ext != NULL)
		*ext = '.';

	dep = new (struct dep);
	dep->next = f->deps;
	dep->path = calloc (2, sizeof (struct path));
	dep->path[0].type = PATH_NAME;
	dep->path[0].name = s;
	dep->path[1].type = PATH_NULL;
	f->deps = dep;
	if (f->dtail == NULL)
		f->dtail = dep;

	f->dtail->next = inf->deps;
	f->rule = inf->rule;
	return 0;
}

struct inference *
find_inf (sc, dir, name)
struct scope *sc;
struct path *dir;
char *name;
{
	extern struct file *try_find ();
	struct inference *inf;
	struct file *sf;
	char *sn, *base;
	char *ext;

	ext = strrchr (name, '.');
	base = strdup (name);
	if (ext == NULL) {
		ext = "";
	} else {
		base[ext - name] = '\0';
	}

	for (inf = sc->dir->infs; inf != NULL; inf = inf->next) {
		if (strcmp (inf->to, ext) == 0) {
			sn = xstrcat (base, inf->from);
			sf = find_file (sc->dir, sn);
			if (sf == NULL)
				sf = try_find (sc, dir, sn);
			free (sn);
			if (sf != NULL)
				break;
		}
	}

	free (base);
	return inf != NULL || sc->parent == NULL ? inf : find_inf (sc->parent, dir, name);
}

struct file *
try_find (sc, dir, name)
struct scope *sc;
struct path *dir;
char *name;
{
	struct inference *inf;
	struct timespec t;
	struct file *f;

	t = get_mtime (dir, name);
	if (t.tv_sec == 0) {
		inf = find_inf (sc, dir, name);
		if (inf == NULL)
			return NULL;
		return inst_inf (sc, inf, name);
	}

	f = new (struct file);
	f->next = sc->dir->files;
	f->name = strdup (name);
	f->rule = NULL;
	f->mtime = t;
	sc->dir->files = f;

	return f;
}

/* BUILDING */

struct timespec
build_file (sc, name, prefix)
struct scope *sc;
char *name;
struct path *prefix;
{
	extern struct timespec build_dir ();
	int needs_update;
	struct scope *sub;
	struct path *new_prefix = NULL;
	struct file *f;
	struct timespec t, maxt;
	struct inference *inf;
	struct expand_ctx ctx;
	struct dep *dep;
	char **s;

	if (verbose) {
		printf ("dir %s", path_to_str (prefix));
		if (name)
			printf (" (%s)", name);
		printf (" ...\n");
	}

	switch (sc->type) {
	case SC_DIR:
		if (sc->dir == NULL)
			parse_dir (sc, prefix);

		if (name == NULL && sc->dir->default_file != NULL)
			name = sc->dir->default_file;

		if (name != NULL) {
			f = find_file (sc->dir, name);
			if (f == NULL) {
				for (sub = sc->dir->subdirs; sub != NULL; sub = sub->next) {
					if (strcmp (sub->name, name) == 0) {
						tmppath.name = name;
						new_prefix = path_cat (prefix, &tmppath);
						t = build_file (sub, NULL, new_prefix);
						goto ret;
					}
				}
				f = try_find (sc, prefix, name);
				if (f == NULL)
					errx (1, "%s: no such file: %s", path_to_str (prefix), name);
			} else {
				f->mtime = get_mtime (prefix, name);
			}
		} else {
			f = sc->dir->files;
			if (f == NULL)
				errx (1, "%s: nothing to build", path_to_str (prefix));
			for (; f->next != NULL; f = f->next);
		}

		if (f->rule == NULL || *f->rule->code == NULL) {
			inf = name != NULL ? find_inf (sc, prefix, name) : NULL;
			if (inf != NULL) {
				inf_inst_file (f, inf);
			} else if (f->rule == NULL) {
				if (f->mtime.tv_sec > 0) {
					t = f->mtime;
					goto ret;
				} else {
					errx (1, "%s: no rule to build: %s", path_to_str (prefix), name);
				}
			}
		}

		needs_update = (f->mtime.tv_sec == 0);
		maxt = f->mtime;

		for (dep = f->deps; dep != NULL; dep = dep->next) {
			t = build_dir (sc, dep->path, prefix);
			if (tv_cmp (&t, &f->mtime) >= 0)
				needs_update = 1;
			if (tv_cmp (&t, &maxt) > 0)
				maxt = t;
		}

		if (!needs_update) {
			t = f->mtime;
			goto ret;
		}

		s = f->rule->code;

		/* rule is a "sum" rule */
		if (s == NULL || *s == NULL) {
			t = maxt;
			goto ret;
		}

		ctx.f = f;

		for (; *s != NULL; ++s) {
			if (runcom (sc, prefix, *s, &ctx) != 0)
				errx (1, "command failed: %s", *s);
		}

		t = f->mtime = now ();
		goto ret;
	case SC_GNU:
		/* first check if the target is already built */
		if (rungnu (sc, prefix, name, 1) == 0) {
			t.tv_sec = 0;
			goto ret;
		}

		if (rungnu (sc, prefix, name, 0) != 0)
			errx (1, "building foreign directory failed");

		t = now ();
		goto ret;
	}
	
ret:
	if (new_prefix != NULL)
		free (new_prefix);
	return t;
}

struct timespec
build_dir (sc, path, prefix)
struct scope *sc;
struct path *path, *prefix;
{
	struct path *new_prefix = NULL;
	struct scope *sub;
	struct timespec t;

	switch (path[0].type) {
	case PATH_SUPER:
		new_prefix = path_cat (prefix, &path[0]);
		t = build_dir (sc->parent, path + 1, new_prefix);
		goto ret;
	case PATH_NULL:
		return build_file (sc, NULL, prefix);
	case PATH_NAME:
		if (path[1].type == PATH_NULL) {
			return build_file (sc, path[0].name, prefix);
		} else {
			if (sc->type != SC_DIR)
				errx (1, "%s: invalid path", path_to_str (prefix));

			if (sc->dir == NULL)
				parse_dir (sc, prefix);

			new_prefix = path_cat (prefix, &path[0]);

			for (sub = sc->dir->subdirs; sub != NULL; sub = sub->next) {
				if (strcmp (sub->name, path[0].name) == 0) {
					t = build_dir (sub, path + 1, new_prefix);
					goto ret;
				}
			}
			
			errx (1, "%s: invalid subdir: %s", path_to_str (prefix), path[0].name);
		}
	}

ret:
	if (new_prefix)
		free (new_prefix);
	return t;
}

struct timespec
build (sc, path)
struct scope *sc;
struct path *path;
{
	return build_dir (sc, path, &path_null);
}

/* HELP */

help_macros (sc)
struct scope *sc;
{
	struct macro *m;

	assert (sc->dir != NULL);

	for (m = sc->dir->macros; m != NULL; m = m->next) {
		if (m->help == NULL)
			continue;

		printf ("%-30s- %s\n", m->name, m->help);
	}

	return 0;
}

help_files (prefix, sc, v)
struct path *prefix;
struct scope *sc;
{
	struct path *new_prefix;
	struct scope *sub;
	struct file *f;
	char *p;
	int n;

	p = path_to_str (prefix);
	if (strcmp (p, ".") == 0) {
		p = NULL;
	} else {
		p += 2; /* skip ./ */
	}

	/* TODO: this should be sorted from top to down */
	for (f = sc->dir->files; f != NULL; f = f->next) {
		if (f->help == NULL)
			continue;

		n = 0;
		if (p != NULL)
			n += printf ("%s/", p);
		n += printf ("%s", f->name);
		printf ("%-*s- %s\n", n < 30 ? 30 - n : 0, "", f->help);
	}

	if (!v)
		return 0;

	for (sub = sc->dir->subdirs; sub != NULL; sub = sub->next) {
		new_prefix = parse_subdir (prefix, sub);
		help_files (new_prefix, sub, v);
		free (new_prefix);
	}

	return 0;
}

help (prefix, sc, v)
struct path *prefix;
struct scope *sc;
{
	extern usage ();
	usage (1);

	fputs ("\nOPTIONS:\n", stderr);
	fputs ("-C dir                        - chdir(dir)\n", stderr);
	fputs ("-f file                       - read `file` instead of \"" MAKEFILE "\"\n", stderr);
	fputs ("-h                            - print help page\n", stderr);
	fputs ("-hv                           - print help page, recursively\n", stderr);
	fputs ("-p                            - dump tree\n", stderr);
	fputs ("-pv                           - dump tree, recursively\n", stderr);
	fputs ("-v                            - verbose output\n", stderr);

	fputs ("\nMACROS:\n", stderr);
	help_macros (sc);

	fputs ("\nTARGETS:\n", stderr);
	help_files (prefix, sc, v);

	return 1;
}

/* DUMP */

print_sc (prefix, sc, verbose)
struct path *prefix;
struct scope *sc;
{
	struct path *new_prefix;
	struct inference *inf;
	struct scope *sub;
	struct macro *m;
	struct dep *dep;
	struct file *f;
	struct rule *r;
	char **s;

	if (verbose)
		printf ("=== %s/%s\n", path_to_str (prefix), sc->makefile);

	if (sc->type != SC_DIR || sc->dir == NULL)
		errx (1, "print_sc(): must be of type SC_DIR");

	for (m = sc->dir->macros; m != NULL; m = m->next) {
		if (m->help != NULL)
			printf ("\n## %s\n", m->help);
		printf ("%s %s= %s\n", m->name, m->prepend != NULL ? "+" : "", m->value);
	}

	printf ("\n");

	for (f = sc->dir->files; f != NULL; f = f->next) {
		if (f->help != NULL)
			printf ("## %s\n", f->help);
		printf ("%s:", f->name);
	
		for (dep = f->deps; dep != NULL; dep = dep->next)
			printf (" %s", path_to_str (dep->path));
		printf ("\n");

		r = f->rule;
		if (r != NULL) {
			for (s = r->code; *s != NULL; ++s)
				printf ("\t%s\n", *s);
		}
		printf ("\n");
	}

	for (inf = sc->dir->infs; inf != NULL; inf = inf->next) {
		printf ("%s%s:", inf->from, inf->to);
		for (dep = inf->deps; dep != NULL; dep = dep->next)
			printf (" %s", path_to_str (dep->path));
		printf ("\n");
		for (s = inf->rule->code; *s != NULL; ++s)
			printf ("\t%s\n", *s);
		printf ("\n");
	}
	
	for (sub = sc->dir->subdirs; sub != NULL; sub = sub->next) {
		switch (sub->type) {
		case SC_DIR:
			printf (".include %s, DIR", sub->name);
			if (sc->makefile != NULL)
				printf (", %s", sc->makefile);
			printf ("\n");
			break;
		case SC_GNU:
			printf (".include %s, GNU", sub->name);
			if (sub->gnu->prog != NULL)
				printf (", %s", sub->gnu->prog);
			if (sc->makefile != NULL)
				printf (", %s", sc->makefile);
			printf ("\n");
			break;
		}
	}

	if (verbose) {
		for (sub = sc->dir->subdirs; sub != NULL; sub = sub->next) {
			if (sub->type != SC_DIR)
				continue;

			printf ("\n");

			new_prefix = parse_subdir (prefix, sub);
			print_sc (new_prefix, sub, verbose);
			free (new_prefix);
		}
	}

	return 0;
}

/* MAIN */

usage (uc)
{
	fprintf (stderr, "%s: %s [-hpv] [-C dir] [-f makefile] [target...]\n", uc ? "USAGE" : "usage", m_make.value);
	return 1;
}

main (argc, argv)
char **argv;
{
	str_t cmdline;
	struct scope *sc;
	struct path *path;
	struct macro *m;
	char *s, *cd = NULL, *makefile = MAKEFILE;
	int i, option, pr = 0, n = 0, dohelp = 0;

	m_dmake.value = m_make.value = argv[0];

	str_new (&cmdline);
	while ((option = getopt (argc, argv, "hpvC:f:")) != -1) {
		switch (option) {
		case 'h':
			dohelp = 1;
			break;
		case 'p':
			str_puts (&cmdline, " -p");
			pr = 1;
			break;
		case 'v':
			str_puts (&cmdline, " -v");
			verbose = 1;
			break;
		case 'C':
			cd = optarg;
			break;
		case 'f':
			makefile = optarg;
			break;
		case '?':
			return usage (0);
		default:
			errx (1, "unexpected option: -%c", option);
		}
	}

	if (cd != NULL && chdir (cd) != 0)
		err (1, "chdir()");

	argv += optind;
	argc -= optind;

	for (i = 0; i < argc; ++i) {
		s = strchr (argv[i], '=');
		if (s == NULL)
			continue;

		str_putc (&cmdline, ' ');
		str_puts (&cmdline, argv[i]);

		*s = '\0';
		m = new (struct macro);
		m->next = globals;
		m->name = trim (argv[i]);
		m->value = trim (s + 1);
		globals = m;

		argv[i] = NULL;
	}

	str_trim (&cmdline);
	m_dmakeflags.value = m_makeflags.value = str_release (&cmdline);

	path = parse_path (".");
	sc = parse_recursive (path, makefile);
	if (sc == NULL)
		errx (1, "failed to find or parse makefile");

	if (dohelp)
		return help (path, sc, verbose);

	if (pr) {
		print_sc (path, sc, verbose);
		return 0;
	}

	free (path);

	for (i = 0; i < argc; ++i) {
		if (argv[i] == NULL)
			continue;

		path = parse_path (argv[i]);
		build (sc, path);
		free (path);
		++n;
	}

	if (n == 0)
		build (sc, &path_null);

	return 0;
}


