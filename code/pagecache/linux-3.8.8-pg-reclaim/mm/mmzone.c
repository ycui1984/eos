/*
 * linux/mm/mmzone.c
 *
 * management codes for pgdats and zones.
 */


#include <linux/stddef.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

struct pglist_data *first_online_pgdat(void)
{
	return NODE_DATA(first_online_node);
}

struct pglist_data *next_online_pgdat(struct pglist_data *pgdat)
{
	int nid = next_online_node(pgdat->node_id);

	if (nid == MAX_NUMNODES)
		return NULL;
	return NODE_DATA(nid);
}

/*
 * next_zone - helper magic for for_each_zone()
 */
struct zone *next_zone(struct zone *zone)
{
	pg_data_t *pgdat = zone->zone_pgdat;

	if (zone < pgdat->node_zones + MAX_NR_ZONES - 1)
		zone++;
	else {
		pgdat = next_online_pgdat(pgdat);
		if (pgdat)
			zone = pgdat->node_zones;
		else
			zone = NULL;
	}
	return zone;
}

static inline int zref_in_nodemask(struct zoneref *zref, nodemask_t *nodes)
{
#ifdef CONFIG_NUMA
	return node_isset(zonelist_node_idx(zref), *nodes);
#else
	return 1;
#endif /* CONFIG_NUMA */
}

/* Returns the next zone at or below highest_zoneidx in a zonelist */
struct zoneref *next_zones_zonelist(struct zoneref *z,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes,
					struct zone **zone)
{
	/*
	 * Find the next suitable zone to use for the allocation.
	 * Only filter based on nodemask if it's set
	 */
	if (likely(nodes == NULL))
		while (zonelist_zone_idx(z) > highest_zoneidx)
			z++;
	else
		while (zonelist_zone_idx(z) > highest_zoneidx ||
				(z->zone && !zref_in_nodemask(z, nodes)))
			z++;

	*zone = zonelist_zone(z);
	return z;
}

#ifdef CONFIG_ARCH_HAS_HOLES_MEMORYMODEL
int memmap_valid_within(unsigned long pfn,
					struct page *page, struct zone *zone)
{
	if (page_to_pfn(page) != pfn)
		return 0;

	if (page_zone(page) != zone)
		return 0;

	return 1;
}
#endif /* CONFIG_ARCH_HAS_HOLES_MEMORYMODEL */

#ifdef CONFIG_RECLAIM_POLICY
/* The default page reclaim policy is LRU2Q */
enum pgreclaim_policy_list global_reclaim_policy = PAGE_RECLAIM_LRU2Q;

struct lruvec {
	/* 
	 * The first field of the lruvec must be a pointer either to a zone or
	 * to a mem_cgroup_per_zone (mz) so we can retrive the zone/mz it
	 * belongs to.
	 */
	union {
		struct zone *zone;
		struct mem_cgroup_per_zone *mz;
	};
};
struct zone *lruvec_zone(struct lruvec *lruvec)
{
	return lruvec->zone;
}

struct mem_cgroup_per_zone *lruvec_mz(struct lruvec *lruvec)
{
	return lruvec->mz;
}

void set_lruvec_mz(struct lruvec *lruvec, struct mem_cgroup_per_zone *mz)
{
	lruvec->mz = mz;
}

void set_lruvec_zone(struct lruvec *lruvec, struct zone *zone)
{
	lruvec->zone = zone;
}

const struct page_reclaim_policy *pgreclaim_policies[NR_PAGE_RECLAIM_POLICIES];
/* For each defined page_reclaim_policy initialise its lruvec. */
void init_lruvecs(struct lruvec *lruvecs[], struct zone *zone,
		  struct mem_cgroup_per_zone *mz)
{
	enum pgreclaim_policy_list p;
	const struct page_reclaim_policy *policy;
	for(p = PGRECLAIM_BASE; p < NR_PAGE_RECLAIM_POLICIES; p++) {
		policy = reclaim_policy(p);
		if (policy) {
			policy->init_lruvec(&lruvecs[p], zone);
			if (zone)
				set_lruvec_zone(lruvecs[p], zone);
			else
				set_lruvec_mz(lruvecs[p], mz);
		}
	}
	printk("init_lruvecs(): called.\n");
}

#ifdef CONFIG_SYSFS

#define PAGE_RECLAIM_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO(_name)
#define PAGE_RECLAIM_ATTR(_name) \
	static struct kobj_attribute _name##_attr = \
		__ATTR(_name, 0644, _name##_show, _name##_store)

static struct zone *get_zone_by_type(long int nid, enum zone_type ztype)
{
	struct zone *zone;
	pg_data_t *pgdat = NODE_DATA(nid);

	if (!pgdat)
		return NULL;
	zone = &pgdat->node_zones[ztype];
	if (!zone)
		return NULL;
	return zone;
}

#ifdef CONFIG_ZONE_DMA
static ssize_t zone_dma_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	long int nid;
	int err;
	struct zone *zone;

	err = strict_strtol(kobj->name, 10, &nid);
	if (err)
		return sprintf(buf, "%d\n", global_reclaim_policy);

	zone = get_zone_by_type(nid, ZONE_DMA);
	if (!zone)
		return sprintf(buf, "%d\n", global_reclaim_policy);

	return sprintf(buf, "%d\n", zone->policy);
}

static ssize_t zone_dma_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	long int policy;
	long int nid;
	int err;
	struct zone *zone;
	char *envp[] = { "dma", NULL};

	err = strict_strtol(kobj->name, 10, &nid);
	if (err)
		return -EINVAL;

	zone = get_zone_by_type(nid, ZONE_DMA);
	if (!zone)
		return -EINVAL;

	err = strict_strtol(buf, 10, &policy);
	if (err)
		return -EINVAL;

	if (policy >= NR_PAGE_RECLAIM_POLICIES)
		policy = PAGE_RECLAIM_LRU2Q;

	printk("zone_dma_store: invoke switching of policies.\n");
	switch_reclaim_policy(zone, zone->policy, policy);
	zone->policy = policy;

	kobject_uevent_env(kobj, KOBJ_CHANGE, envp);
	return count;
}
PAGE_RECLAIM_ATTR(zone_dma);
#endif /* CONFIG_ZONE_DMA */

#ifdef CONFIG_ZONE_DMA32
static ssize_t zone_dma32_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	long int nid;
	int err;
	struct zone *zone;

	err = strict_strtol(kobj->name, 10, &nid);
	if (err)
		return sprintf(buf, "%d\n", global_reclaim_policy);

	zone = get_zone_by_type(nid, ZONE_DMA32);
	if (!zone)
		return sprintf(buf, "%d\n", global_reclaim_policy);

	return sprintf(buf, "%d\n", zone->policy);
}

static ssize_t zone_dma32_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	long int policy;
	long int nid;
	int err;
	struct zone *zone;
	char *envp[] = { "dma32", NULL};

	err = strict_strtol(kobj->name, 10, &nid);
	if (err)
		return -EINVAL;

	zone = get_zone_by_type(nid, ZONE_DMA32);
	if (!zone)
		return -EINVAL;

	err = strict_strtol(buf, 10, &policy);
	if (err)
		return -EINVAL;

	if (policy >= NR_PAGE_RECLAIM_POLICIES)
		policy = PAGE_RECLAIM_LRU2Q;

	printk("zone_dma32_store: invoke switching of policies.\n");
	switch_reclaim_policy(zone, zone->policy, policy);
	zone->policy = policy;

	kobject_uevent_env(kobj, KOBJ_CHANGE, envp);
	return count;
}
PAGE_RECLAIM_ATTR(zone_dma32);
#endif /* CONFIG_ZONE_DMA32 */

#ifdef CONFIG_HIGHMEM
static ssize_t zone_highmem_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	long int nid;
	int err;
	struct zone *zone;

	err = strict_strtol(kobj->name, 10, &nid);
	if (err)
		return sprintf(buf, "%d\n", global_reclaim_policy);

	zone = get_zone_by_type(nid, ZONE_HIGHMEM);
	if (!zone)
		return sprintf(buf, "%d\n", global_reclaim_policy);

	return sprintf(buf, "%d\n", zone->policy);
}

static ssize_t zone_highmem_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	long int policy;
	long int nid;
	int err;
	struct zone *zone;
	char *envp[] = { "highmem", NULL};

	err = strict_strtol(kobj->name, 10, &nid);
	if (err)
		return -EINVAL;

	zone = get_zone_by_type(nid, ZONE_HIGHMEM);
	if (!zone)
		return -EINVAL;

	err = strict_strtol(buf, 10, &policy);
	if (err)
		return -EINVAL;

	if (policy >= NR_PAGE_RECLAIM_POLICIES)
		policy = PAGE_RECLAIM_LRU2Q;

	printk("zone_highmem_store: invoke switching of policies.\n");
	switch_reclaim_policy(zone, zone->policy, policy);
	zone->policy = policy;

	kobject_uevent_env(kobj, KOBJ_CHANGE, envp);
	return count;
}
PAGE_RECLAIM_ATTR(zone_highmem);
#endif /* CONFIG_HIGHMEM */

static ssize_t zone_normal_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	long int nid;
	int err;
	struct zone *zone;

	err = strict_strtol(kobj->name, 10, &nid);
	if (err)
		return sprintf(buf, "%d\n", global_reclaim_policy);

	zone = get_zone_by_type(nid, ZONE_NORMAL);
	if (!zone)
		return sprintf(buf, "%d\n", global_reclaim_policy);

	return sprintf(buf, "%d\n", zone->policy);
}

static ssize_t zone_normal_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	long int policy;
	long int nid;
	int err;
	struct zone *zone;
	char *envp[] = { "normal", NULL};

	err = strict_strtol(kobj->name, 10, &nid);
	if (err)
		return -EINVAL;

	zone = get_zone_by_type(nid, ZONE_NORMAL);
	if (!zone)
		return -EINVAL;

	err = strict_strtol(buf, 10, &policy);
	if (err)
		return -EINVAL;

	if (policy >= NR_PAGE_RECLAIM_POLICIES)
		policy = PAGE_RECLAIM_LRU2Q;

	preempt_disable();
	printk(KERN_ERR "zone_normal_store: invoke switching of policies.\n");
	switch_reclaim_policy(zone, zone->policy, policy);
	zone->policy = policy;
	
	preempt_enable();

	kobject_uevent_env(kobj, KOBJ_CHANGE, envp);
	return count;
}
PAGE_RECLAIM_ATTR(zone_normal);

static ssize_t zone_movable_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	long int nid;
	int err;
	struct zone *zone;

	err = strict_strtol(kobj->name, 10, &nid);
	if (err)
		return sprintf(buf, "%d\n", global_reclaim_policy);

	zone = get_zone_by_type(nid, ZONE_MOVABLE);
	if (!zone)
		return sprintf(buf, "%d\n", global_reclaim_policy);

	return sprintf(buf, "%d\n", zone->policy);
}

static ssize_t zone_movable_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	long int policy;
	long int nid;
	int err;
	struct zone *zone;
	char *envp[] = { "movable", NULL};

	err = strict_strtol(kobj->name, 10, &nid);
	if (err)
		return -EINVAL;

	zone = get_zone_by_type(nid, ZONE_MOVABLE);
	if (!zone)
		return -EINVAL;

	err = strict_strtol(buf, 10, &policy);
	if (err)
		return -EINVAL;

	if (policy >= NR_PAGE_RECLAIM_POLICIES)
		policy = PAGE_RECLAIM_LRU2Q;

	printk("zone_movable_store: invoke switching of policies.\n");
	switch_reclaim_policy(zone, zone->policy, policy);
	zone->policy = policy;

	kobject_uevent_env(kobj, KOBJ_CHANGE, envp);
	return count;
}
PAGE_RECLAIM_ATTR(zone_movable);

static struct attribute *pgreclaim_attrs[] = {
#ifdef CONFIG_ZONE_DMA
	&zone_dma_attr.attr,
#endif
#ifdef CONFIG_ZONE_DMA32
	&zone_dma32_attr.attr,
#endif
	&zone_normal_attr.attr,
#ifdef CONFIG_HIGHMEM
	&zone_highmem_attr.attr,
#endif
	&zone_movable_attr.attr,
	NULL,
};

static struct attribute_group pgreclaim_attr_group = {
	.attrs = pgreclaim_attrs,
};

static struct kset *pgreclaim_kset;

static void pgreclaim_kset_init(void)
{
	int nid;
	int err;
	struct kobject *kobj;
	pgreclaim_kset = kset_create_and_add("page_reclaim", NULL, mm_kobj);
	if (!pgreclaim_kset) {
		return;
	}
	for_each_online_node(nid) {
		/* create a kobject and add it. */
		kobj = kobject_create();
		if (!kobj)
			continue;
		kobj->kset = pgreclaim_kset;

		err = kobject_add(kobj, NULL, "%d", nid);
		if (err) {
			printk(KERN_WARNING "%s: kobject_add error: %d\n",
				__func__, err);
			kobject_put(kobj);
			kobj = NULL;
			continue;
		}

		err = sysfs_create_group(kobj, &pgreclaim_attr_group);
		if (err) {
			kobject_del(kobj);
			kobject_put(kobj);
			continue;
		}
	}
}

static ssize_t pgreclaim_policy_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", global_reclaim_policy);
}

static ssize_t pgreclaim_policy_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	long int policy;
	int err;
	char *envp[] = {"global_policy", NULL};

	err = strict_strtol(buf, 10, &policy);
	if (err)
		return -EINVAL;
	if (policy >= NR_PAGE_RECLAIM_POLICIES)
		policy = PAGE_RECLAIM_LRU2Q;

	printk("pgreclaim_policy_store: invoke switching of policies.\n");

	switch_reclaim_policy_all(global_reclaim_policy, policy);
	global_reclaim_policy = policy;

	kobject_uevent_env(kobj, KOBJ_CHANGE, envp);
	return count;
}
PAGE_RECLAIM_ATTR(pgreclaim_policy);

static struct attribute *global_pgreclaim_attrs[] = {
	&pgreclaim_policy_attr.attr,
	NULL,
};

static struct attribute_group global_pgreclaim_attr_group = {
	.attrs = global_pgreclaim_attrs,
	.name = "page_reclaim_global",
};

static int __init pgreclaim_sysfs_init(void)
{
	int err;
	err = sysfs_create_group(mm_kobj, &global_pgreclaim_attr_group);
	if (err) {
		printk(KERN_ERR "page_reclaim: register sysfs failed\n");
		return err;
	}

	/* Create attribute for each zone in the memory. */
	pgreclaim_kset_init();

	return 0;
}
late_initcall(pgreclaim_sysfs_init);

#endif /* CONFIG_SYSFS */

void init_page_reclaim_policies(void) {
	pgreclaim_policies[PAGE_RECLAIM_LRU2Q] = &lru_page_reclaim_policy;
	pgreclaim_policies[PAGE_RECLAIM_FIFO] = &fifo_page_reclaim_policy;
	pgreclaim_policies[PAGE_RECLAIM_CLOCKPRO] = &clockpro_page_reclaim_policy;
	pgreclaim_policies[PAGE_RECLAIM_ARC] = &arc_page_reclaim_policy;
	pgreclaim_policies[PAGE_RECLAIM_CART] = &cart_page_reclaim_policy;
}

void init_history_lists(struct list_head *history_list)
{
	int i;
	memset(history_list, 0, NR_HISTORY_LISTS * sizeof(struct list_head));
	for (i = HISTORY_EVICTABLE; i < NR_HISTORY_LISTS; i++) {
		INIT_LIST_HEAD(&history_list[i]);
	}
}
#else
void lruvec_init(struct lruvec *lruvec)
{
	enum lru_list lru;

	memset(lruvec, 0, sizeof(struct lruvec));

	for_each_lru(lru)
		INIT_LIST_HEAD(&lruvec->lists[lru]);
}
#endif

