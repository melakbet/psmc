#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include "psmc.h"

void psmc_resamp(psmc_par_t *pp)
{
	int i, n_seqs = 0;
	psmc_seq_t *seqs = 0;
	int64_t L = 0, L_ori = 0;
	for (i = 0; i != pp->n_seqs; ++i) L_ori += pp->seqs[i].L;
	while (1) {
		psmc_seq_t *ns, *s = pp->seqs + (int)(pp->n_seqs * drand48());
		int tmp1 = L_ori - L;
		int tmp2 = L + s->L - L_ori;
		if (tmp2 <= 0 || (tmp2 > 0 && tmp1 > 0 && tmp2 < tmp1)) { // add seq
			if ((n_seqs&0xff) == 0)
				seqs = (psmc_seq_t*)realloc(seqs, sizeof(psmc_seq_t) * (n_seqs + 0x100));
			ns = seqs + n_seqs;
			ns->name = strdup(s->name);
			ns->seq = (char*)malloc(s->L);
			memcpy(ns->seq, s->seq, s->L);
			ns->L = s->L;
			ns->L_e = s->L_e;
			ns->n_e = s->n_e;
			L += ns->L;
			++n_seqs;
		}
		if (tmp1 >= 0 && tmp2 >= 0) break;
	}
	// delete old information
	for (i = 0; i != pp->n_seqs; ++i) {
		free(pp->seqs[i].name);
		free(pp->seqs[i].seq);
	}
	free(pp->seqs);
	// fill up new information
	pp->n_seqs = n_seqs;
	pp->seqs = seqs;
	pp->sum_n = pp->sum_L = 0;
	for (i = 0; i != pp->n_seqs; ++i) {
		pp->sum_n += pp->seqs[i].n_e;
		pp->sum_L += pp->seqs[i].L_e;
	}
}

void psmc_print_data(const psmc_par_t *pp, const psmc_data_t *pd)
{
	int k;
	FLOAT n_recomb = pp->sum_L / pd->C_sigma;
	FLOAT theta0, rho0, *lambda, sum;
	lambda = (FLOAT*)malloc(sizeof(FLOAT) * (pp->n + 1));
	theta0 = pd->params[0]; rho0 = pd->params[1];
	for (k = 0; k <= pp->n; ++k)
		lambda[k] = pd->params[pp->par_map[k] + 2];
	fprintf(pp->fpout, "LK\t%lf\n", pd->lk);
	fprintf(pp->fpout, "QD\t%lf -> %lf\n", pd->Q0, pd->Q1);
	// calculate Relative Information (KL distnace)
	for (k = 0, sum = 0.0; k <= pp->n; ++k)
		sum += pd->sigma[k] * log(pd->sigma[k] / pd->post_sigma[k]);
	fprintf(pp->fpout, "RI\t%.10lf\n", sum);
	fprintf(pp->fpout, "TR\t%lf\t%lf\n", pd->params[0], pd->params[1]);
	fprintf(pp->fpout, "MM\tC_pi: %lf, n_recomb: %lf\n", pd->C_pi, n_recomb);
	for (k = 0; k <= pp->n; ++k)
		fprintf(pp->fpout, "RS\t%d\t%lf\t%lf\t%lf\t%lf\t%lf\n", k, pd->t[k], lambda[k], n_recomb * pd->hp->a0[k],
				pd->sigma[k], pd->post_sigma[k]);
	fprintf(pp->fpout, "PA\t%s", pp->pattern);
	for (k = 0; k != pd->n_params; ++k)
		fprintf(pp->fpout, " %.9lf", pd->params[k]);
	for (k = 1; k <= pp->n; ++k)
		fprintf(pp->fpout, " %.9lf", pd->t[k]);
	fprintf(pp->fpout, "\n//\n");
	fflush(pp->fpout);
	free(lambda);
}
void psmc_read_param(psmc_par_t *pp)
{
	FILE *fp;
	char str[256];
	int k;
	if (pp->pre_fn == 0) return;
	assert(fp = fopen(pp->pre_fn, "r"));
	fscanf(fp, "%s", str);
	if (pp->pattern) free(pp->pattern);
	pp->pattern = (char*)malloc(strlen(str) + 1);
	strcpy(pp->pattern, str);
	if (pp->par_map) free(pp->par_map);
	pp->par_map = psmc_parse_pattern(pp->pattern, &pp->n_free, &pp->n);
	/* initialize inp_pa and inp_ti */
	pp->inp_ti = (FLOAT*)malloc(sizeof(FLOAT) * (pp->n + 2));
	pp->inp_pa = (FLOAT*)malloc(sizeof(FLOAT) * (pp->n_free + 2));
	for (k = 0; k != pp->n_free + 2; ++k)
		fscanf(fp, "%lf", pp->inp_pa + k);
	pp->inp_ti[0] = 0.0;
	for (k = 1; k <= pp->n; ++k)
		fscanf(fp, "%lf", pp->inp_ti + k);
	pp->inp_ti[pp->n + 1] = PSMC_T_INF;
	/* for other stuff */
	pp->max_t = pp->inp_ti[pp->n];
	pp->tr_ratio = pp->inp_pa[0] / pp->inp_pa[1];
	fclose(fp);
}

void psmc_decode(const psmc_par_t *pp, const psmc_data_t *pd)
{
	hmm_par_t *hp = pd->hp;
	int i, k, prev, start;
	FLOAT p, q, *t, *t2, *t_min, *invCov = 0, *marginal_stddev = 0;
	t = (FLOAT*)malloc(sizeof(FLOAT) * (pp->n + 1));
	for (k = 0; k <= pp->n; ++k) {
		t[k] = (pd->t[k] + 1.0 - (pd->t[k+1] - pd->t[k]) / (exp(pd->t[k+1]) / exp(pd->t[k]) - 1.0)) / pd->C_pi;
		if (pp->is_fulldec) fprintf(pp->fpout, "TC\t%d\t%lf\t%lf\t%lf\n", k, t[k], pd->t[k], pd->t[k+1]);
	}
	t2 = (FLOAT*)malloc(sizeof(FLOAT) * pp->n_free);
	t_min = (FLOAT*)malloc(sizeof(FLOAT) * pp->n_free);
	t_min[0] = 0;
	for (k = i = 0, p = 0; k < pp->n_free; ++k) {
		for (; i < pp->n; ++i) if (pp->par_map[i] == k) break;
		t_min[k] = pd->t[i];
		prev = i;
		for (; i < pp->n; ++i) if (pp->par_map[i] > k) break;
		t2[k] = (pd->t[prev] + 1.0 - (pd->t[i] - pd->t[prev]) / (exp(pd->t[i]) / exp(pd->t[prev]) - 1.0)) / pd->C_pi;
	}
	hmm_pre_backward(hp);
	for (i = 0; i != pp->n_seqs; ++i) {
		hmm_data_t *hd;
		psmc_seq_t *s = pp->seqs + i;
		char *seq = (char*)calloc(s->L+1, 1);
		memcpy(seq, s->seq, s->L);
		hd = hmm_new_data(s->L, seq, hp);
		hmm_forward(hp, hd);
		hmm_backward(hp, hd);
		if (!pp->is_fulldec && pp->is_decoding) { // posterior decoding
			int *x, kl;
			hmm_post_decode(hp, hd);
			/* show path */
			x = hd->p;
			start = 1; prev = x[1];
			p = hd->f[1][prev] * hd->b[1][prev] * hd->s[1];
			for (k = 2; k <= s->L; ++k) {
				if (prev != x[k]) {
					kl = pp->par_map[prev];
					fprintf(pp->fpout, "DC\t%s\t%d\t%d\t%d\t%.5f\t%.5f\t%.5f\n", s->name, start, k-1, kl,
							t_min[kl], t2[kl], kl == pp->n_free-1? pp->max_t * 2. : t_min[kl+1]);
//					fprintf(pp->fpout, "DC\t%s\t%d\t%d\t%d\t%.3lf\t%.2lf\n", s->name, start, k-1, prev, t[prev], p);
					prev = x[k]; start = k; p = 0.0;
				}
				q = hd->f[k][x[k]] * hd->b[k][x[k]] * hd->s[k];
				if (p < q) p = q;
			}
//			fprintf(pp->fpout, "DC\t%s\t%d\t%d\t%d\t%.3lf\t%.2lf\n", s->name, start, k-1, prev, t[prev], p);
			kl = pp->par_map[prev];
			fprintf(pp->fpout, "DC\t%s\t%d\t%d\t%d\t%.5f\t%.5f\t%.5f\n", s->name, start, k-1, kl,
					t_min[kl], t2[kl], kl == pp->n_free-1? pp->max_t * 2. : t_min[kl+1]);
			fflush(pp->fpout);
		} else if (pp->is_decoding) { // full decoding
			FLOAT *prob = (FLOAT*)malloc(sizeof(FLOAT) * hp->n);
			for (k = 1; k <= s->L; ++k) {
				int l;
				FLOAT p, *fu, *bu1, *eu1;
				if (k < s->L) {
					p = 0.0; fu = hd->f[k]; bu1 = hd->b[k+1]; eu1 = hp->e[(int)hd->seq[k+1]];
					for (l = 0; l < hp->n; ++l)
						p += fu[l] * hp->a[l][l] * bu1[l] * eu1[l];
					p = 1.0 - p;
				} else p = 0.0;
				hmm_post_state(hp, hd, k, prob);
				fprintf(pp->fpout, "DF\t%d\t%lf", k, p);
				for (l = 0; l < hp->n; ++l)
					fprintf(pp->fpout, "\t%.4f", prob[l]);
				fprintf(pp->fpout, "\n");
			}
		}
		/* free */
		hmm_delete_data(hd);
		free(seq);
	}
	free(t); free(t2); free(t_min); free(invCov); free(marginal_stddev);
}

void psmc_simulate(const psmc_par_t *pp, const psmc_data_t *pd)
{
	const char conv[3] = { 'T', 'K', 'N' };
	int i, k;
	srand48(time(0));
	for (i = 0; i != pp->n_seqs; ++i) {
		psmc_seq_t *s = pp->seqs + i;
		char *seq;
		seq = hmm_simulate(pd->hp, s->L);
		for (k = 0; k != s->L; ++k)
			seq[k] = (s->seq[k] == 2)? 'N' : conv[(int)seq[k]];
		// print
		fprintf(pp->fpout, "FA\t>%s", s->name);
		for (k = 0; k != s->L; ++k) {
			if (k%60 == 0) fprintf(pp->fpout, "\nFA\t");
			fputc(seq[k], pp->fpout);
		}
		fputc('\n', pp->fpout);
		free(seq);
	}
}
