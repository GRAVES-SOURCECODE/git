/*
 * "git fast-rebase" builtin command
 *
 * FAST: Forking Any Subprocesses (is) Taboo
 *
 * This is meant SOLELY as a demo of what is possible.  sequencer.c and
 * rebase.c should be refactored to use the ideas here, rather than attempting
 * to extend this file to replace those (unless Phillip or Dscho say that
 * refactoring is too hard and we need a clean slate, but I'm guessing that
 * refactoring is the better route).
 */

#define USE_THE_INDEX_COMPATIBILITY_MACROS
#include "builtin.h"

#include "argv-array.h"
#include "cache-tree.h"
#include "commit.h"
#include "lockfile.h"
#include "merge-ort.h"
#include "refs.h"
#include "revision.h"
#include "sequencer.h"
#include "tree.h"

static const char *short_commit_name(struct commit *commit)
{
	return find_unique_abbrev(&commit->object.oid, DEFAULT_ABBREV);
}

static struct commit *peel_committish(const char *name)
{
	struct object *obj;
	struct object_id oid;

	if (get_oid(name, &oid))
		return NULL;
	obj = parse_object(the_repository, &oid);
	return (struct commit *)peel_to_type(name, 0, obj, OBJ_COMMIT);
}

static char *get_author(const char *message)
{
	size_t len;
	const char *a;

	a = find_commit_header(message, "author", &len);
	if (a)
		return xmemdupz(a, len);

	return NULL;
}

static struct commit *create_commit(struct tree *tree,
				    struct commit *based_on,
				    struct commit *parent)
{
	struct object_id ret;
	struct object *obj;
	struct commit_list *parents = NULL;
	char *author;
	char *sign_commit = NULL;
	struct commit_extra_header *extra;
	struct strbuf msg = STRBUF_INIT;
	const char *out_enc = get_commit_output_encoding();
	const char *message = logmsg_reencode(based_on, NULL, out_enc);
	const char *orig_message = NULL;
	const char *exclude_gpgsig[] = { "gpgsig", NULL };

	commit_list_insert(parent, &parents);
	extra = read_commit_extra_headers(based_on, exclude_gpgsig);
	find_commit_subject(message, &orig_message);
	strbuf_addstr(&msg, orig_message);
	author = get_author(message);
	reset_ident_date();
	if (commit_tree_extended(msg.buf, msg.len, &tree->object.oid, parents,
				 &ret, author, sign_commit, extra)) {
		error(_("failed to write commit object"));
		return NULL;
	}

	obj = parse_object(the_repository, &ret);
	return (struct commit *)obj;
}

int cmd_fast_rebase(int argc, const char **argv, const char *prefix)
{
	struct commit *onto;
	struct commit *last_commit = NULL, *last_picked_commit = NULL;
	struct object_id head;
	struct lock_file lock = LOCK_INIT;
	int clean = 1;
	struct argv_array rev_walk_args = ARGV_ARRAY_INIT;
	struct rev_info revs;
	struct commit *commit;
	struct merge_options merge_opt;
	struct tree *next_tree, *base_tree, *head_tree;
	struct merge_result result;
	struct strbuf reflog_msg = STRBUF_INIT;
	struct strbuf branch_name = STRBUF_INIT;

	if (argc != 5 || strcmp(argv[1], "--onto"))
		die("usage: read the code, figure out how to use it, then do so");
	onto = peel_committish(argv[2]);
	strbuf_addf(&branch_name, "refs/heads/%s", argv[4]);

	/* Sanity check */
	if (get_oid("HEAD", &head))
		die(_("Cannot read HEAD"));
	assert(oideq(&onto->object.oid, &head));

	hold_locked_index(&lock, LOCK_DIE_ON_ERROR);
	assert(repo_read_index(the_repository) >= 0);

	repo_init_revisions(the_repository, &revs, NULL);
	revs.verbose_header = 1;
	revs.max_parents = 1;
	revs.cherry_mark = 1;
	revs.limited = 1;
	revs.reverse = 1;
	revs.right_only = 1;
	revs.sort_order = REV_SORT_IN_GRAPH_ORDER;
	revs.topo_order = 1;
	argv_array_pushl(&rev_walk_args, "", argv[4], "--not", argv[3], NULL);

	if (setup_revisions(rev_walk_args.argc, rev_walk_args.argv, &revs,
			    NULL) > 1)
		return error(_("unhandled options"));

	if (prepare_revision_walk(&revs) < 0)
		return error(_("error preparing revisions"));

	init_merge_options(&merge_opt, the_repository);
	memset(&result, 0, sizeof(result));
	merge_opt.show_rename_progress = 1;
	merge_opt.branch1 = "HEAD";
	head_tree = get_commit_tree(onto);
	result.automerge_tree = head_tree;
	last_commit = onto;
	while ((commit = get_revision(&revs))) {
		struct commit *base;

		fprintf(stderr, "Rebasing %s...\r",
			oid_to_hex(&commit->object.oid));
		assert(commit->parents && !commit->parents->next);
		base = commit->parents->item;

		next_tree = get_commit_tree(commit);
		base_tree = get_commit_tree(base);

		merge_opt.branch2 = short_commit_name(commit);
		merge_opt.ancestor = xstrfmt("parent of %s", merge_opt.branch2);

		merge_ort_inmemory_nonrecursive(&merge_opt,
						result.automerge_tree,
						next_tree,
						base_tree,
						&result);

		if (!result.clean)
			die("Aborting: Hit a conflict and restarting is not implemented.");
		last_picked_commit = commit;
		last_commit = create_commit(result.automerge_tree, commit,
					    last_commit);
	}
	fprintf(stderr, "\nDone.\n");

	if (switch_to_merge_result(&merge_opt, head_tree, &result))
		clean = -1;
	merge_finalize(&merge_opt, &result);

	if (clean < 0)
		exit(128);

	strbuf_addf(&reflog_msg, "finish rebase %s onto %s",
		    oid_to_hex(&last_picked_commit->object.oid),
		    oid_to_hex(&last_commit->object.oid));
	if (update_ref(reflog_msg.buf, branch_name.buf,
		       &last_commit->object.oid,
		       &last_picked_commit->object.oid,
		       REF_NO_DEREF, UPDATE_REFS_MSG_ON_ERR)) {
		error(_("could not update %s"), argv[4]);
		die("Failed to update %s", argv[4]);
	}
	strbuf_reset(&reflog_msg);
	strbuf_addf(&reflog_msg, "finish rebase %s onto %s",
		    oid_to_hex(&last_picked_commit->object.oid),
		    oid_to_hex(&last_commit->object.oid));
	if (create_symref("HEAD", branch_name.buf, reflog_msg.buf) < 0)
		die(_("unable to update HEAD"));

	prime_cache_tree(the_repository, the_repository->index,
			 result.automerge_tree);
	if (write_locked_index(&the_index, &lock,
			       COMMIT_LOCK | SKIP_IF_UNCHANGED))
		die(_("unable to write %s"), get_index_file());
	return (clean == 0);
}