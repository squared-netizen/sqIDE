/* sqterm: a #! interpreter by a path Android has not (/bin/sh, /usr/bin/env,
 * /bin/bash) is looked up by its name on PATH, where sqterm's prefix comes
 * first and /system/bin after it. Included into Src/exec.c by the recipe
 * (packages/zsh/recipe.sh), and used where zsh already looks up an
 * interpreter named without a path. Termux does the same for every program
 * with termux-exec; this covers what zsh runs. */
static char *
sqterm_interpreter(char *prog)
{
    char *name = prog;

    if (*prog == '/') {
	/* It is there: execve failed for another reason, and zsh's own
	 * "bad interpreter" message says so. */
	if (access(prog, X_OK) == 0)
	    return NULL;
	name = strrchr(prog, '/') + 1;
    }
    return *name ? pathprog(name, NULL) : NULL;
}
