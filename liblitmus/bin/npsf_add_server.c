/* wrapper for sys_add_server
 *
 * Input: a file with on each line:
 * npsf_id cpu budget(us)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "litmus.h"
#include "common.h"

void usage(char *error) {
	fprintf(stderr, "Error: %s\n", error);
	fprintf(stderr,
		"Usage: npsf_add_server SERVERS-FILE MAX-SPLITS-PER-NPSFID\n");
	exit(1);
}

int main(int argc, char** argv)
{
	int ret;
	FILE *file;
	int i,j;
	int npsf_id, curr_id = -1;
	int cpu, max_splits_server;
	int budget_us;
	struct npsf_budgets *budgets;

	if (argc < 3)
		usage("Arguments missing.");

	max_splits_server = atoi(argv[2]);

	if ((file = fopen(argv[1], "r")) == NULL) {
		fprintf(stderr, "Cannot open %s\n", argv[1]);
		return -1;
	}

	/* format: npsf_id cpu budget-us */
	i = 0;
	while (fscanf(file, "%d %d %d\n", &npsf_id, &cpu, &budget_us) != EOF) {

		printf("Read: %d %d %d\n", npsf_id, cpu, budget_us);

		if (curr_id == -1) {
			curr_id = npsf_id;
			budgets = malloc(max_splits_server *
					sizeof(struct npsf_budgets));
			for(j = 0; j < max_splits_server; j++) {
				budgets[j].cpu = -1;
				budgets[j].budget = 0;
			}
		}

		if (npsf_id == curr_id) {
			/* same notional processor, different cpu and budget */
			budgets[i].cpu = cpu;
			budgets[i].budget = (lt_t) (budget_us * 1000);
			i++;
		} else {
			/* different notional processor */
			/* add server */
			printf("Adding npsf_id = %d\n", curr_id);
			ret = add_server(&curr_id, budgets, 0);

			if (ret < 0) {
				fclose(file);
				free(budgets);
				printf("Cannot add Notional Processor %d\n",
						curr_id);
				return ret;
			}

			/* reinit new */
			i = 0;
			budgets = malloc(max_splits_server *
					sizeof(struct npsf_budgets));
			for(j = 0; j < max_splits_server; j++) {
				budgets[j].cpu = -1;
				budgets[j].budget = 0;
			}
			curr_id = npsf_id;
			budgets[i].cpu = cpu;
			budgets[i].budget = (lt_t) (budget_us * 1000);
			i++;
		}
	}

	if (ferror(file)) {
		fprintf(stderr, "Error while reading\n");
		fclose(file);
		return -1;
	}

	/* save the last entry */
	ret = add_server(&curr_id, budgets, 1);
	printf("Adding npsf_id = %d\n", curr_id);
	if (ret < 0) {
		fclose(file);
		free(budgets);
		bail_out("Cannot add Notional Processor: ");
	}

	fclose(file);

	return 0;
}
