
int find_free() {
	for (int j = 0; j < MAX_PMEM_SIZE/PAGESIZE; j++) {
		if (frames[j] == 0) return j;
	}
	return ERROR;
}



static void free_region1(pte_t *pt) {
    for (int i = 0; i < MAX_PT_LEN; i++) {
        if (pt[i].valid && i >= (VMEM_1_BASE >> PAGESHIFT)) {
            frames[pt[i].pfn] = 0;
            pt[i].valid = 0;
        }
    }
}
