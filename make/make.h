#ifndef FILE_MAKE_H
#define FILE_MAKE_H
#include <sys/time.h>

enum path_type {
	PATH_NULL,
	PATH_SUPER,
	PATH_NAME,
};
struct path {
	enum path_type type;
	char *name;
};

struct template {
	struct template *next;
	char *name;
	char *text;
};

enum scope_type {
	SC_DIR,
	SC_GNU,
	SC_CUSTOM,
};
struct scope {
	struct scope *next;
	enum scope_type type;
	char *name; /* optional */
	struct scope *parent; /* optional */
	char *makefile; /* required */
	union {
		struct directory *dir; /* optional */
		struct gnu *gnu; /* required */
		struct custom *custom; /* required */
	};
};

struct directory {
	struct scope *subdirs;
	struct file *files;
	struct macro *macros;
	struct macro *emacros;	/* exported macros */
	struct inference *infs;
	struct template *templates;
	char *default_file;
	int done;
};

struct gnu {
	char *prog;	/* optional (default: "make") */
};

struct custom {
	struct file *test, *exec;
};

struct dep {
	struct dep *next;
	struct path *path;
};

struct file {
	struct file *next;
	char *name;
	struct rule *rule; /* optional */
	struct dep *deps, *dtail;
	struct timespec mtime;
	char *help; /* optional */
};

struct inference {
	struct inference *next;
	char *from, *to;
	struct rule *rule;
	struct dep *deps, *dtail;
};

struct rule {
	char **code;
};

struct macro {
	struct macro *next, *enext, *prepend;
	char *name;
	char *value;
	char *help;
	int lazy;
};

#endif /* FILE_MAKE_H */
