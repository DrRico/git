#include "config.h"
static int diff_indent_heuristic = 1;
static int diff_color_moved_default;
	GIT_COLOR_DI_IT_MAGENTA,/* OLD_MOVED */
	GIT_COLOR_BG_RED,	/* OLD_MOVED ALTERNATIVE */
	GIT_COLOR_DI_IT_CYAN,	/* NEW_MOVED */
	GIT_COLOR_BG_GREEN,	/* NEW_MOVED ALTERNATIVE */
	if (!strcasecmp(var, "oldmoved"))
		return DIFF_FILE_OLD_MOVED;
	if (!strcasecmp(var, "oldmovedalternative"))
		return DIFF_FILE_OLD_MOVED_ALT;
	if (!strcasecmp(var, "newmoved"))
		return DIFF_FILE_NEW_MOVED;
	if (!strcasecmp(var, "newmovedalternative"))
		return DIFF_FILE_NEW_MOVED_ALT;
static int parse_color_moved(const char *arg)
{
	if (!strcmp(arg, "no"))
		return MOVED_LINES_NO;
	else if (!strcmp(arg, "nobounds"))
		return MOVED_LINES_BOUNDARY_NO;
	else if (!strcmp(arg, "allbounds"))
		return MOVED_LINES_BOUNDARY_ALL;
	else if (!strcmp(arg, "adjacentbounds"))
		return MOVED_LINES_BOUNDARY_ADJACENT;
	else if (!strcmp(arg, "alternate"))
		return MOVED_LINES_ALTERNATE;
	else
		return -1;
}

	if (!strcmp(var, "diff.colormoved")) {
		int cm = parse_color_moved(value);
		if (cm < 0)
			return -1;
		diff_color_moved_default = cm;
		return 0;
	}
	if (git_diff_heuristic_config(var, value, cb) < 0)
		return -1;

struct moved_entry {
	struct hashmap_entry ent;
	const struct diff_line *line;
	struct moved_entry *next_line;
};

static void get_ws_cleaned_string(const struct diff_line *l,
				  struct strbuf *out)
{
	int i;
	for (i = 0; i < l->len; i++) {
		if (isspace(l->line[i]))
			continue;
		strbuf_addch(out, l->line[i]);
	}
}

static int diff_line_cmp_no_ws(const struct diff_line *a,
					 const struct diff_line *b,
					 const void *keydata)
{
	int ret;
	struct strbuf sba = STRBUF_INIT;
	struct strbuf sbb = STRBUF_INIT;

	get_ws_cleaned_string(a, &sba);
	get_ws_cleaned_string(b, &sbb);
	ret = sba.len != sbb.len || strncmp(sba.buf, sbb.buf, sba.len);

	strbuf_release(&sba);
	strbuf_release(&sbb);
	return ret;
}

static int diff_line_cmp(const struct diff_line *a,
				   const struct diff_line *b,
				   const void *keydata)
{
	return a->len != b->len || strncmp(a->line, b->line, a->len);
}

static int moved_entry_cmp(const struct moved_entry *a,
			   const struct moved_entry *b,
			   const void *keydata)
{
	return diff_line_cmp(a->line, b->line, keydata);
}

static int moved_entry_cmp_no_ws(const struct moved_entry *a,
				 const struct moved_entry *b,
				 const void *keydata)
{
	return diff_line_cmp_no_ws(a->line, b->line, keydata);
}

static unsigned get_line_hash(struct diff_line *line, unsigned ignore_ws)
{
	static struct strbuf sb = STRBUF_INIT;

	if (ignore_ws) {
		strbuf_reset(&sb);
		get_ws_cleaned_string(line, &sb);
		return memhash(sb.buf, sb.len);
	} else {
		return memhash(line->line, line->len);
	}
}

static struct moved_entry *prepare_entry(struct diff_options *o,
					 int line_no)
{
	struct moved_entry *ret = xmalloc(sizeof(*ret));
	unsigned ignore_ws = DIFF_XDL_TST(o, IGNORE_WHITESPACE);
	struct diff_line *l = &o->line_buffer[line_no];

	ret->ent.hash = get_line_hash(l, ignore_ws);
	ret->line = l;
	ret->next_line = NULL;

	return ret;
}

static void add_lines_to_move_detection(struct diff_options *o,
					struct hashmap *add_lines,
					struct hashmap *del_lines)
	struct moved_entry *prev_line = NULL;

	int n;
	for (n = 0; n < o->line_buffer_nr; n++) {
		int sign = 0;
		struct hashmap *hm;
		struct moved_entry *key;

		switch (o->line_buffer[n].sign) {
		case '+':
			sign = '+';
			hm = add_lines;
			break;
		case '-':
			sign = '-';
			hm = del_lines;
			break;
		case ' ':
		default:
			prev_line = NULL;
			continue;
		}

		key = prepare_entry(o, n);
		if (prev_line &&
		    prev_line->line->sign == sign)
			prev_line->next_line = key;

		hashmap_add(hm, key);
		prev_line = key;
	}
}

static void mark_color_as_moved_single_line(struct diff_options *o,
					    struct diff_line *l, int alt_color)
{
	switch (l->sign) {
	case '+':
		l->set = diff_get_color_opt(o,
			DIFF_FILE_NEW_MOVED + alt_color);
		break;
	case '-':
		l->set = diff_get_color_opt(o,
			DIFF_FILE_OLD_MOVED + alt_color);
		break;
	default:
		die("BUG: we should have continued earlier?");
	}
}

static void mark_color_as_moved(struct diff_options *o,
				struct hashmap *add_lines,
				struct hashmap *del_lines)
{
	struct moved_entry **pmb = NULL; /* potentially moved blocks */
	struct diff_line *prev_line = NULL;
	int pmb_nr = 0, pmb_alloc = 0;
	int n, flipped_block = 0;

	for (n = 0; n < o->line_buffer_nr; n++) {
		struct hashmap *hm = NULL;
		struct moved_entry *key;
		struct moved_entry *match = NULL;
		struct diff_line *l = &o->line_buffer[n];
		int i, lp, rp, adjacent_blocks = 0;

		/* Check for any match to color it as a move. */
		switch (l->sign) {
		case '+':
			hm = del_lines;
			key = prepare_entry(o, n);
			match = hashmap_get(hm, key, o);
			free(key);
			break;
		case '-':
			hm = add_lines;
			key = prepare_entry(o, n);
			match = hashmap_get(hm, key, o);
			free(key);
			break;
		default: ;
			flipped_block = 0;
		}

		if (!match) {
			pmb_nr = 0;
			if (prev_line &&
			    o->color_moved == MOVED_LINES_BOUNDARY_ALL)
				mark_color_as_moved_single_line(o, prev_line, 1);
			prev_line = NULL;
			continue;
		}

		if (o->color_moved == MOVED_LINES_BOUNDARY_NO) {
			mark_color_as_moved_single_line(o, l, 0);
			continue;
		}

		/* Check any potential block runs, advance each or nullify */
		for (i = 0; i < pmb_nr; i++) {
			struct moved_entry *p = pmb[i];
			struct moved_entry *pnext = (p && p->next_line) ?
					p->next_line : NULL;
			if (pnext &&
			    !diff_line_cmp(pnext->line, l, o)) {
				pmb[i] = p->next_line;
			} else {
				pmb[i] = NULL;
			}
		}

		/* Shrink the set of potential block to the remaining running */
		for (lp = 0, rp = pmb_nr - 1; lp <= rp;) {
			while (lp < pmb_nr && pmb[lp])
				lp++;
			/* lp points at the first NULL now */

			while (rp > -1 && !pmb[rp])
				rp--;
			/* rp points at the last non-NULL */

			if (lp < pmb_nr && rp > -1 && lp < rp) {
				pmb[lp] = pmb[rp];
				pmb[rp] = NULL;
				rp--;
				lp++;
			}
		}

		/* Remember the number of running sets */
		pmb_nr = rp + 1;

		if (pmb_nr == 0) {
			/*
			 * This line is the start of a new block.
			 * Setup the set of potential blocks.
			 */
			for (; match; match = hashmap_get_next(hm, match)) {
				ALLOC_GROW(pmb, pmb_nr + 1, pmb_alloc);
				pmb[pmb_nr++] = match;
			}

			if (o->color_moved == MOVED_LINES_BOUNDARY_ALL) {
				adjacent_blocks = 1;
			} else {
				/* Check if two blocks are adjacent */
				adjacent_blocks = prev_line &&
						  prev_line->sign == l->sign;
			}
		}

		if (o->color_moved == MOVED_LINES_ALTERNATE) {
			if (adjacent_blocks)
				flipped_block = (flipped_block + 1) % 2;
			mark_color_as_moved_single_line(o, l, flipped_block);
		} else {
			/* MOVED_LINES_BOUNDARY_{ADJACENT, ALL} */
			mark_color_as_moved_single_line(o, l, adjacent_blocks);
			if (adjacent_blocks && prev_line)
				prev_line->set = l->set;
		}

		prev_line = l;
	}
	if (prev_line && o->color_moved == MOVED_LINES_BOUNDARY_ALL)
		mark_color_as_moved_single_line(o, prev_line, 1);

	free(pmb);
}

static void emit_diff_line(struct diff_options *o,
			   struct diff_line *e)
{
	const char *ws;
	int len = e->len;
	if (e->add_line_prefix)
		fputs(diff_line_prefix(o), file);

	switch (e->state) {
	case DIFF_LINE_WS:
		ws = diff_get_color(o->use_color, DIFF_WHITESPACE);
		if (e->set)
			fputs(e->set, file);
		if (e->sign)
			fputc(e->sign, file);
		if (e->reset)
			fputs(e->reset, file);
		ws_check_emit(e->line, e->len, o->ws_rule,
			      file, e->set, e->reset, ws);
		return;
	case DIFF_LINE_ASIS:
		has_trailing_newline = (len > 0 && e->line[len-1] == '\n');
		has_trailing_carriage_return = (len > 0 && e->line[len-1] == '\r');
		if (len || e->sign) {
			if (e->set)
				fputs(e->set, file);
			if (e->sign)
				fputc(e->sign, file);
			fwrite(e->line, len, 1, file);
			if (e->reset)
				fputs(e->reset, file);
		}
		if (has_trailing_carriage_return)
			fputc('\r', file);
		if (has_trailing_newline)
			fputc('\n', file);
		return;
	case DIFF_LINE_RELOAD_WS_RULE:
		o->ws_rule = whitespace_rule(e->line);
		return;
	default:
		die("BUG: malformatted buffered patch line: '%d'", e->state);
static void append_diff_line(struct diff_options *o,
			     struct diff_line *e)
	struct diff_line *f;
	ALLOC_GROW(o->line_buffer,
		   o->line_buffer_nr + 1,
		   o->line_buffer_alloc);
	f = &o->line_buffer[o->line_buffer_nr++];

	memcpy(f, e, sizeof(struct diff_line));
	f->line = e->line ? xmemdupz(e->line, e->len) : NULL;
}

static void emit_line(struct diff_options *o,
		      const char *set, const char *reset,
		      int add_line_prefix, int markup_ws,
		      int sign, const char *line, int len)
{
	struct diff_line e = {set, reset, line,
		len, sign, add_line_prefix,
		markup_ws ? DIFF_LINE_WS : DIFF_LINE_ASIS};

	if (o->use_buffer)
		append_diff_line(o, &e);
	else
		emit_diff_line(o, &e);
}

static void emit_line_fmt(struct diff_options *o,
			  const char *set, const char *reset,
			  int add_line_prefix,
			  const char *fmt, ...)
{
	struct strbuf sb = STRBUF_INIT;
	va_list ap;
	va_start(ap, fmt);
	strbuf_vaddf(&sb, fmt, ap);
	va_end(ap);

	emit_line(o, set, reset, add_line_prefix, 0, 0, sb.buf, sb.len);
	strbuf_release(&sb);
}

void diff_emit_line(struct diff_options *o, const char *set, const char *reset,
		    const char *line, int len)
{
	emit_line(o, set, reset, 1, 0, 0, line, len);
		emit_line(ecbdata->opt, set, reset, 1, 0, sign, line, len);
		emit_line(ecbdata->opt, ws, reset, 1, 1, sign, line, len);
		emit_line(ecbdata->opt, set, reset, 1, 1, sign, line, len);

		emit_line(ecbdata->opt, context, reset, 1, 0, 0, line, len);
	strbuf_complete_line(&msgbuf);

	emit_line(ecbdata->opt, "", "", 1, 0, 0, msgbuf.buf, msgbuf.len);
static void add_line_count(struct strbuf *out, int count)
		strbuf_addstr(out, "0,0");
		strbuf_addstr(out, "1");
		strbuf_addf(out, "1,%d", count);
	struct strbuf sb = STRBUF_INIT;
		const char *endp = memchr(data, '\n', size);
		if (endp)
			len = endp - data + 1;
		else {
			strbuf_add(&sb, data, size);
			strbuf_addch(&sb, '\n');
			size = 0; /* to exit the loop. */

			data = sb.buf;
			len = sb.len;
		}
	if (sb.len) {
		static const char *nneof = "\\ No newline at end of file\n";
		emit_line(ecb->opt, context, reset, 1, 0, 0,
		strbuf_release(&sb);
	struct strbuf out = STRBUF_INIT;
	emit_line_fmt(o, metainfo, reset, 1, "--- %s%s\n", a_name.buf, name_a_tab);
	emit_line_fmt(o, metainfo, reset, 1, "+++ %s%s\n", b_name.buf, name_b_tab);


	strbuf_addstr(&out, "@@ -");
		add_line_count(&out, lc_a);
		strbuf_addstr(&out, "?,?");
	strbuf_addstr(&out, " +");
	add_line_count(&out, lc_b);
	strbuf_addstr(&out, " @@\n");
	emit_line(o, fraginfo, reset, 1, 0, 0, out.buf, out.len);
	strbuf_release(&out);

static int fn_out_diff_words_write_helper(struct diff_options *o,
					  size_t count, const char *buf)
	struct strbuf sb = STRBUF_INIT;
			emit_line(o, NULL, NULL, 1, 0, 0, "", 0);

			const char *reset = st_el->color && *st_el->color ?
					    GIT_COLOR_RESET : NULL;
			strbuf_addstr(&sb, st_el->prefix);
			strbuf_add(&sb, buf, p ? p - buf : count);
			strbuf_addstr(&sb, st_el->suffix);
			emit_line(o, st_el->color, reset,
				  0, 0, 0, sb.buf, sb.len);
			strbuf_reset(&sb);
			goto out;

		strbuf_addstr(&sb, newline);
		emit_line(o, NULL, NULL, 0, 0, 0, sb.buf, sb.len);
		strbuf_reset(&sb);

out:
	strbuf_release(&sb);
		fn_out_diff_words_write_helper(diff_words->opt,
				diff_words->current_plus);
		fn_out_diff_words_write_helper(diff_words->opt,
				minus_end - minus_begin, minus_begin);
		fn_out_diff_words_write_helper(diff_words->opt,
				plus_end - plus_begin, plus_begin);
		fn_out_diff_words_write_helper(diff_words->opt,
			diff_words->minus.text.ptr);
			emit_line(diff_words->opt, NULL, NULL, 1, 0, 0, "", 0);
		fn_out_diff_words_write_helper(diff_words->opt,
			- diff_words->current_plus, diff_words->current_plus);

	if (ecbdata->diff_words->opt->line_buffer_nr) {
		int i;
		for (i = 0; i < ecbdata->diff_words->opt->line_buffer_nr; i++)
			append_diff_line(ecbdata->opt,
				&ecbdata->diff_words->opt->line_buffer[i]);

		for (i = 0; i < ecbdata->diff_words->opt->line_buffer_nr; i++)
			free((void *)ecbdata->diff_words->opt->line_buffer[i].line);

		ecbdata->diff_words->opt->line_buffer_nr = 0;
	}

	o->line_buffer = NULL;
	o->line_buffer_nr = 0;
	o->line_buffer_alloc = 0;

		free (ecbdata->diff_words->opt->line_buffer);
		FREE_AND_NULL(ecbdata->diff_words);
		emit_line(o, NULL, NULL, 0, 0, 0,
			  ecbdata->header->buf, ecbdata->header->len);
		strbuf_release(ecbdata->header);
		emit_line_fmt(o, meta, reset, 1, "--- %s%s\n",
			      ecbdata->label_path[0], name_a_tab);
		emit_line_fmt(o, meta, reset, 1, "+++ %s%s\n",
			      ecbdata->label_path[1], name_b_tab);
			emit_line(o, context, reset, 1, 0, 0, line, len);
			emit_line(o, NULL, NULL, 0, 0, 0, "~\n", 2);
			emit_line(o, context, reset, 1, 0, 0, line, len);
			  reset, 1, 0, 0, line, len);
static void show_name(struct strbuf *out,
	strbuf_addf(out, " %s%-*s |", prefix, len, name);
static void show_graph(struct strbuf *out, char ch, int cnt, const char *set, const char *reset)
	strbuf_addstr(out, set);
	strbuf_addchars(out, ch, cnt);
	strbuf_addstr(out, reset);
static void print_stat_summary_0(struct diff_options *options, int files,
				 int insertions, int deletions)
		strbuf_addstr(&sb, " 0 files changed");
		emit_line(options, NULL, NULL, 1, 0, 0, sb.buf, sb.len);
		return;
	emit_line(options, NULL, NULL, 1, 0, 0, sb.buf, sb.len);
}

void print_stat_summary(FILE *fp, int files,
			int insertions, int deletions)
{
	struct diff_options o;
	memset(&o, 0, sizeof(o));
	o.file = fp;
	print_stat_summary_0(&o, files, insertions, deletions);
	const char *line_prefix = diff_line_prefix(options);
	struct strbuf out = STRBUF_INIT;
			show_name(&out, prefix, name, len);
			strbuf_addf(&out, " %*s", number_width, "Bin");
				strbuf_addch(&out, '\n');
				emit_line(options, NULL, NULL, 1, 0, 0, out.buf, out.len);
				strbuf_reset(&out);
			strbuf_addf(&out, " %s%"PRIuMAX"%s",
			strbuf_addstr(&out, " -> ");
			strbuf_addf(&out, "%s%"PRIuMAX"%s",
			strbuf_addstr(&out, " bytes\n");
			emit_line(options, NULL, NULL, 1, 0, 0, out.buf, out.len);
			strbuf_reset(&out);
			show_name(&out, prefix, name, len);
			strbuf_addstr(&out, " Unmerged\n");
			emit_line(options, NULL, NULL, 1, 0, 0, out.buf, out.len);
			strbuf_reset(&out);
		show_name(&out, prefix, name, len);
		strbuf_addf(&out, " %*"PRIuMAX"%s",
		show_graph(&out, '+', add, add_c, reset);
		show_graph(&out, '-', del, del_c, reset);
		strbuf_addch(&out, '\n');
		emit_line(options, NULL, NULL, 1, 0, 0, out.buf, out.len);
		strbuf_reset(&out);
			emit_line(options, NULL, NULL, 1, 0, 0,
				  " ...\n", strlen(" ...\n"));

	print_stat_summary_0(options, total_files, adds, dels);
		int deleted = data->files[i]->deleted;
	print_stat_summary_0(options, total_files, adds, dels);
		emit_line(data->o, set, reset, 1, 0, 0, line, 1);
static void emit_binary_diff_body(struct diff_options *o,
				  mmfile_t *one, mmfile_t *two)
		emit_line_fmt(o, NULL, NULL, 1, "delta %lu\n", orig_size);
	} else {
		emit_line_fmt(o, NULL, NULL, 1, "literal %lu\n", two->size);
		int len;
		char line[71];

		len = strlen(line);
		line[len++] = '\n';
		line[len] = '\0';

		emit_line(o, NULL, NULL, 1, 0, 0, line, len);
	emit_line(o, NULL, NULL, 1, 0, 0, "\n", 1);
static void emit_binary_diff(struct diff_options *o,
			     mmfile_t *one, mmfile_t *two)
	const char *s = "GIT binary patch\n";
	const int len = strlen(s);
	emit_line(o, NULL, NULL, 1, 0, 0, s, len);
	emit_binary_diff_body(o, one, two);
	emit_binary_diff_body(o, two, one);
		show_submodule_summary(o, one->path ? one->path : two->path,
		show_submodule_inline_diff(o, one->path ? one->path : two->path,
				meta, del, add, reset);
			emit_line(o, NULL, NULL, 0, 0, 0, header.buf, header.len);
		emit_line(o, NULL, NULL, 0, 0, 0, header.buf, header.len);
					emit_line(o, NULL, NULL, 0, 0, 0,
						  header.buf, header.len);
			emit_line(o, NULL, NULL, 0, 0, 0,
				  header.buf, header.len);
			emit_line_fmt(o, NULL, NULL, 1,
				      "Binary files %s and %s differ\n",
				      lbl[0], lbl[1]);
				emit_line(o, NULL, NULL, 0, 0, 0,
					  header.buf, header.len);
		emit_line(o, NULL, NULL, 0, 0, 0,
			  header.buf, header.len);
			emit_binary_diff(o, &mf1, &mf2);
			emit_line_fmt(o, NULL, NULL, 1,
				      "Binary files %s and %s differ\n",
				      lbl[0], lbl[1]);
			emit_line(o, NULL, NULL, 0, 0, 0, header.buf, header.len);
		o->ws_rule = ecbdata.ws_rule;
		if (o->use_buffer) {
			struct diff_line e = DIFF_LINE_INIT;
			e.state = DIFF_LINE_RELOAD_WS_RULE;
			e.line = name_b;
			e.len = strlen(name_b);
			append_diff_line(o, &e);
		}
void fill_filespec(struct diff_filespec *spec, const struct object_id *oid,
		   int oid_valid, unsigned short mode)
		oidcpy(&spec->oid, oid);
		spec->oid_valid = oid_valid;
static int reuse_worktree_file(const char *name, const struct object_id *oid, int want_file)
	if (!FAST_WORKING_DIRECTORY && !want_file && has_sha1_pack(oid->hash))
	if (!want_file && would_convert_to_git(&the_index, name))
	if (oidcmp(oid, &ce->oid) || !S_ISREG(ce->ce_mode))
	    reuse_worktree_file(s->path, &s->oid, 0)) {
		if (size_only && !would_convert_to_git(&the_index, s->path))
		if (convert_to_git(&the_index, s->path, s->data, s->size, &buf, crlf_warn)) {
	FREE_AND_NULL(s->cnt_data);
	     reuse_worktree_file(name, &one->oid, 1))) {
				oid_to_hex_r(temp->hex, &null_oid);
			 * !(one->oid_valid), as long as
static void diff_fill_oid_info(struct diff_filespec *one)
	name  = one->path;
	other = (strcmp(name, two->path) ? two->path : NULL);
	diff_fill_oid_info(one);
	diff_fill_oid_info(two);
	diff_fill_oid_info(p->one);
	diff_fill_oid_info(p->two);
	diff_fill_oid_info(p->one);
	diff_fill_oid_info(p->two);

	options->line_buffer = NULL;
	options->line_buffer_nr = 0;
	options->line_buffer_alloc = 0;

	options->color_moved = diff_color_moved_default;

	if (!options->use_color || external_diff())
		options->color_moved = 0;
	else if (!strcmp(arg, "--color-moved"))
		if (diff_color_moved_default)
			options->color_moved = diff_color_moved_default;
		else
			options->color_moved = MOVED_LINES_BOUNDARY_ADJACENT;
	else if (!strcmp(arg, "--no-color-moved"))
		options->color_moved = MOVED_LINES_NO;
	else if (skip_prefix(arg, "--color-moved=", &arg)) {
		int cm = parse_color_moved(arg);
		if (cm < 0)
			die("bad --color-moved argument: %s", arg);
		options->color_moved = cm;
	} else if (!strcmp(arg, "--color-words")) {
		options->file = xfopen(path, "w");
static void show_file_mode_name(struct diff_options *opt, const char *newdelete, struct diff_filespec *fs)
	struct strbuf sb = STRBUF_INIT;
		strbuf_addf(&sb, " %s mode %06o ", newdelete, fs->mode);
		strbuf_addf(&sb, " %s ", newdelete);
	quote_c_style(fs->path, &sb, NULL, 0);
	strbuf_addch(&sb, '\n');
	emit_line(opt, NULL, NULL, 1, 0, 0, sb.buf, sb.len);
	strbuf_release(&sb);
}
static void show_mode_change(struct diff_options *opt, struct diff_filepair *p,
		int show_name)
		struct strbuf sb = STRBUF_INIT;
			strbuf_addch(&sb, ' ');
			quote_c_style(p->two->path, &sb, NULL, 0);
		emit_line_fmt(opt, NULL, NULL, 1,
			      " mode change %06o => %06o%s\n",
			      p->one->mode, p->two->mode,
			      show_name ? sb.buf : "");
		strbuf_release(&sb);
static void show_rename_copy(struct diff_options *opt, const char *renamecopy,
		struct diff_filepair *p)
	emit_line_fmt(opt, NULL, NULL, 1, " %s %s (%d%%)\n",
		      renamecopy, names, similarity_index(p));
	show_mode_change(opt, p, 0);
		show_file_mode_name(opt, "delete", p->one);
		show_file_mode_name(opt, "create", p->two);
		show_rename_copy(opt, "copy", p);
		show_rename_copy(opt, "rename", p);
			struct strbuf sb = STRBUF_INIT;
			strbuf_addstr(&sb, " rewrite ");
			quote_c_style(p->two->path, &sb, NULL, 0);
			strbuf_addf(&sb, " (%d%%)\n", similarity_index(p));
			emit_line(opt, NULL, NULL, 1, 0, 0, sb.buf, sb.len);
		show_mode_change(opt, p, !p->score);
static int diff_get_patch_id(struct diff_options *options, struct object_id *oid, int diff_header_only)
		diff_fill_oid_info(p->one);
		diff_fill_oid_info(p->two);
					GIT_SHA1_HEXSZ);
					GIT_SHA1_HEXSZ);
	git_SHA1_Final(oid->hash, &ctx);
int diff_flush_patch_id(struct diff_options *options, struct object_id *oid, int diff_header_only)
	int result = diff_get_patch_id(options, oid, diff_header_only);
static void diff_flush_patch_all_file_pairs(struct diff_options *o)
{
	int i;
	struct diff_queue_struct *q = &diff_queued_diff;

	if (o->color_moved)
		o->use_buffer = 1;

	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];
		if (check_pair_status(p))
			diff_flush_patch(p, o);
	}

	if (o->use_buffer) {
		if (o->color_moved) {
			struct hashmap add_lines, del_lines;
			unsigned ignore_ws = DIFF_XDL_TST(o, IGNORE_WHITESPACE);

			hashmap_init(&del_lines, ignore_ws ?
				(hashmap_cmp_fn)moved_entry_cmp_no_ws :
				(hashmap_cmp_fn)moved_entry_cmp, 0);
			hashmap_init(&add_lines, ignore_ws ?
				(hashmap_cmp_fn)moved_entry_cmp_no_ws :
				(hashmap_cmp_fn)moved_entry_cmp, 0);

			add_lines_to_move_detection(o, &add_lines, &del_lines);
			mark_color_as_moved(o, &add_lines, &del_lines);

			hashmap_free(&add_lines, 0);
			hashmap_free(&del_lines, 0);
		}

		for (i = 0; i < o->line_buffer_nr; i++)
			emit_diff_line(o, &o->line_buffer[i]);

		for (i = 0; i < o->line_buffer_nr; i++)
			free((void *)o->line_buffer[i].line);

		free(o->line_buffer);

		o->line_buffer = NULL;
		o->line_buffer_nr = 0;
		o->line_buffer_alloc = 0;
	}
}

		options->file = xfopen("/dev/null", "w");
		options->color_moved = 0;
			char term[2];
			term[0] = options->line_termination;
			term[1] = '\0';

			emit_line(options, NULL, NULL, 1, 0, 0, term, !!term[0]);
				emit_line(options, NULL, NULL, 0, 0, 0,
					  options->stat_sep,
					  strlen(options->stat_sep));
		diff_flush_patch_all_file_pairs(options);
		    const struct object_id *oid,
		    int oid_valid,
		fill_filespec(one, oid, oid_valid, mode);
		fill_filespec(two, oid, oid_valid, mode);
		 const struct object_id *old_oid,
		 const struct object_id *new_oid,
		 int old_oid_valid, int new_oid_valid,
		SWAP(old_oid, new_oid);
		SWAP(old_oid_valid, new_oid_valid);
	fill_filespec(one, old_oid, old_oid_valid, old_mode);
	fill_filespec(two, new_oid, new_oid_valid, new_mode);
					  &df->oid,
		notes_cache_put(driver->textconv_cache, &df->oid, *outbuf,
int textconv_object(const char *path,
		    unsigned mode,
		    const struct object_id *oid,
		    int oid_valid,
		    char **buf,
		    unsigned long *buf_size)
{
	struct diff_filespec *df;
	struct userdiff_driver *textconv;

	df = alloc_filespec(path);
	fill_filespec(df, oid, oid_valid, mode);
	textconv = get_textconv(df);
	if (!textconv) {
		free_filespec(df);
		return 0;
	}

	*buf_size = fill_textconv(textconv, df, buf);
	free_filespec(df);
	return 1;
}
