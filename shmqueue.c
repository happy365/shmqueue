#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "rtailq.h"
#include "shmqueue.h"

/* lock/unlock */
static inline void shmqueue_lockinit(struct shmqueue *);
static inline void shmqueue_lock(struct shmqueue *);
static inline void shmqueue_unlock(struct shmqueue *);


/* strings to hash */
uint32_t
shmqueue_hash_string(uint8_t *key)
{
	uint32_t hash = 0;
	uint8_t *p, c;

	p = key;

	while ((c = *p++) != 0)
		hash = (hash << 5) - hash + c;

	return hash;
}

static inline void
shmqueue_lockinit(struct shmqueue *header)
{
	__cpu_simple_lock_init(&header->header->shmqueue_cpulock);
}

static inline void
shmqueue_lock(struct shmqueue *header)
{
	__cpu_simple_lock(&header->header->shmqueue_cpulock);
}

static inline void
shmqueue_unlock(struct shmqueue *header)
{
	__cpu_simple_unlock(&header->header->shmqueue_cpulock);
}

void
shmqueue_dumpitem(struct shmqueue_item *item)
{
	printf("      shi_hashidx=%d\n", item->shi_hashidx);
	printf("      shi_keyvalue.key=<%s>\n", item->shi_keyvalue.kv_key);
	printf("      shi_keyvalue.storagesize=%u\n", item->shi_keyvalue.kv_storagesize);
	printf("      shi_keyvalue.storage=<%s>\n", item->shi_keyvalue.kv_storage);
}

void
shmqueue_dump(struct shmqueue *shmq_header)
{
	struct shmqueue_header *header;

	header = shmq_header->header;

	printf("queue usage: %u/%u %2.3g%%\n",
	    header->sha_item_inuse,
	    header->sha_itemnum,
	    (double)header->sha_item_inuse * 100.0 / header->sha_itemnum);
}

void
shmqueue_dumpall(struct shmqueue *shmq_header)
{
	struct shmqueue_header *header;
	struct shmqueue_item *item;
	int i;

	header = shmq_header->header;


	printf("======== FREELIST ========\n");
	RTAILQ_DUMP_HEAD(&header->sha_freelist, shmqueue_item, "");
	for (item = RTAILQ_FIRST(&header->sha_freelist, shmqueue_item);
	    item != NULL;
	    item = RTAILQ_NEXT(item, &header->sha_freelist, shmqueue_item, shi_list)) {

		RTAILQ_DUMP(item, &header->sha_freelist, shmqueue_item, shi_list, "");
	}

	printf("======== LRULIST ========\n");
	RTAILQ_DUMP_HEAD(&header->sha_lrulist, shmqueue_item, "");
	for (item = RTAILQ_FIRST(&header->sha_lrulist, shmqueue_item);
	    item != NULL;
	    item = RTAILQ_NEXT(item, &header->sha_lrulist, shmqueue_item, shi_list)) {

		RTAILQ_DUMP(item, &header->sha_lrulist, shmqueue_item, shi_list, "");
		shmqueue_dumpitem(item);
	}

	printf("======== HASHLIST ========\n");
	for (i = 0; i < header->sha_hashnum; i++) {
		if (RTAILQ_EMPTY(&header->sha_hashlist[i]))
			continue;

		printf("    ======== HASHLIST[%d] ========\n", i);
		RTAILQ_DUMP_HEAD(&header->sha_hashlist[i], shmqueue_item, "    ");
		for (item = RTAILQ_FIRST(&header->sha_hashlist[i], shmqueue_item);
		    item != NULL;
		    item = RTAILQ_NEXT(item, &header->sha_hashlist[i], shmqueue_item, shi_hashlist)) {

			RTAILQ_DUMP(item, &header->sha_hashlist[i], shmqueue_item, shi_hashlist, "    ");
			shmqueue_dumpitem(item);
		}
	}
}

/* constructor */
struct shmqueue *
shmqueue_new(int shmid, size_t size, int reuse, int verbose)
{
	struct shmqueue *shmqueue;
	void *mem;
	int id, rc;

	if (size == 0) {
		id = shmget(shmid, size, 0);
	} else {
		id = shmget(shmid, size,
#ifdef SHM_HUGETLB
//		    SHM_HUGETLB |
#endif
		    IPC_EXCL | IPC_CREAT | 0666);
		reuse = 0;
	}
	if (id == -1)
		return NULL;

	mem = shmat(id, NULL, 0);
	if (mem == NULL)
		return NULL;

	if (verbose) {
		printf("shmid = %d\n", id);
	}

	shmqueue = malloc(sizeof(struct shmqueue));
	if (shmqueue == NULL)
		return NULL;

	if (!reuse) {
		if (verbose) {
			printf("create memory: %p[%llu]\n", mem, (unsigned long long)size);
		}
		if (shmqueue_create(shmqueue, mem, size) < 0) {
			rc = shmctl(id, IPC_RMID, NULL);
			return NULL;
		}
	} else {
		if (shmqueue_attach(shmqueue, mem) < 0) {
			rc = shmctl(id, IPC_RMID, NULL);
			return NULL;
		}
		if (verbose) {
			printf("attach memory: %p[%llu]\n", mem, (unsigned long long)shmqueue->header->sha_memsize);
		}
	}
	return shmqueue;
}


int
shmqueue_attach(struct shmqueue *shmq_header, void *mem)
{
	shmq_header->header = (struct shmqueue_header *)mem;
	shmq_header->hash_callback = shmqueue_hash_string;
	return 0;
}

int
shmqueue_create(struct shmqueue *shmq_header, void *mem, size_t size)
{
	struct shmqueue_header *header;
	void *memend;
	struct shmqueue_item *itempool;
	size_t headersize, freespace;
	int i;

	memend = (char *)mem + size;

	header = (struct shmqueue_header *)mem;
	headersize = sizeof(struct shmqueue_header);	/* temporary */
#ifdef SHMQUEUE_HASHNUM
	header->sha_hashnum = SHMQUEUE_HASHNUM;
#else
	freespace = size - headersize;
	header->sha_hashnum = freespace / sizeof(struct shmqueue_item);
#endif
	headersize += header->sha_hashnum * sizeof(header->sha_hashlist[0]);

	itempool = (struct shmqueue_item *)SHMQUEUE_ALIGN(
	    (char *)mem + headersize, SHMQUEUE_ALIGNSIZE);

	if ((char *)memend < (char *)itempool) {
		printf("cannot allocate hash table\n");
		return -1;
	}

	freespace = (char *)memend - (char *)itempool;

	header->sha_memsize = size;
	header->sha_itemsize = sizeof(struct shmqueue_item);
	header->sha_itemnum = freespace / sizeof(struct shmqueue_item);
	header->sha_pool = itempool;

	printf("=============================================\n");
	printf("sizeof header = %d\n", (int)sizeof(struct shmqueue_header));
	printf("\n");
	printf("header              = %p\n", header);
	printf("size                = %llu\n", (unsigned long long)size);
	printf("headersize          = %d\n", (int)sizeof(struct shmqueue_header));
	printf("hashentrysize       = %d (%.1g%%)\n",
	    (int)sizeof(struct sha_hashlist) * header->sha_hashnum,
	    (sizeof(struct sha_hashlist) * header->sha_hashnum) * 100.0 / size);
	printf("freespace           = %llu\n", (unsigned long long)freespace);
	printf("header->sha_itemsize= %u\n", header->sha_itemsize);
	printf("header->sha_itemnum = %u\n", header->sha_itemnum);
	printf("header->sha_hashnum = %u\n", header->sha_hashnum);
	printf("header->sha_pool    = %p\n", header->sha_pool);
	printf("=============================================\n");


	/* init queue headers */
	RTAILQ_INIT(&header->sha_freelist);
	RTAILQ_INIT(&header->sha_lrulist);

	for (i = 0; i < header->sha_hashnum; i++)
		RTAILQ_INIT(&header->sha_hashlist[i]);


	/* insert all itempool to freelist */
	header->sha_item_inuse = 0;
	for (i = 0; i < header->sha_itemnum; i++) {
		RTAILQ_INSERT_TAIL(&header->sha_freelist,
		    &itempool[i], shmqueue_item, shi_list);
	}

	shmq_header->header = header;
	shmq_header->hash_callback = shmqueue_hash_string;
	shmqueue_lockinit(shmq_header);

	return 0;
}

void
shmqueue_setcallback(struct shmqueue *shmq_header, uint32_t (*hash_callback)(uint8_t *))
{
	shmq_header->hash_callback = hash_callback;
}


struct shmqueue_item *
shmqueue_remove_from_freelist(struct shmqueue *shmq_header)
{
	struct shmqueue_header *header;
	struct shmqueue_item *item;

	header = shmq_header->header;

	shmqueue_lock(shmq_header);

	item = RTAILQ_FIRST(&header->sha_freelist, shmqueue_item);
	if (item == NULL) {
		shmqueue_unlock(shmq_header);
		return NULL;
	}

	/* remove from FREE list */
	RTAILQ_REMOVE(&header->sha_freelist,
	    item, shmqueue_item, shi_list);

	shmqueue_unlock(shmq_header);

	item->shi_hashidx = 0;
	item->shi_keyvalue.kv_storagesize = 0;

	return item;
}

int
shmqueue_add_to_lru(struct shmqueue *shmq_header, struct shmqueue_item *item, uint32_t hashidx)
{
	struct shmqueue_header *header;

	header = shmq_header->header;
	item->shi_hashidx = hashidx;

	shmqueue_lock(shmq_header);

	/* insert to LRU list */
	RTAILQ_INSERT_HEAD(&header->sha_lrulist,
	    item, shmqueue_item, shi_list);

	/* insert to HASH list */
	RTAILQ_INSERT_HEAD(&header->sha_hashlist[item->shi_hashidx],
	    item, shmqueue_item, shi_hashlist);

	header->sha_item_inuse++;

	shmqueue_unlock(shmq_header);

	return 0;
}

struct shmqueue_item *
shmqueue_remove_from_lru(struct shmqueue *shmq_header)
{
	struct shmqueue_header *header;
	struct shmqueue_item *item;

	header = shmq_header->header;

	shmqueue_lock(shmq_header);

	/* get tail of LRU list */
	item = RTAILQ_LAST(&header->sha_lrulist, shmqueue_item);
	if (item == NULL) {
		shmqueue_unlock(shmq_header);
		return NULL;
	}

	/* remove from LRU list */
	RTAILQ_REMOVE(&header->sha_lrulist,
	    item, shmqueue_item, shi_list);

	/* remove from HASH list */
	RTAILQ_REMOVE(&header->sha_hashlist[item->shi_hashidx],
	    item, shmqueue_item, shi_hashlist);

	header->sha_item_inuse--;

	shmqueue_unlock(shmq_header);

	return item;
}

void
shmqueue_add_to_freelist(struct shmqueue *shmq_header, struct shmqueue_item *item)
{
	struct shmqueue_header *header;

	header = shmq_header->header;

	shmqueue_lock(shmq_header);

	/* insert to FREE list */
	RTAILQ_INSERT_HEAD(&header->sha_freelist,
	    item, shmqueue_item, shi_list);

	shmqueue_unlock(shmq_header);
}

const struct keyvalue *
shmqueue_keyvalue_peek(struct shmqueue *shmq_header, uint8_t *key)
{
	struct shmqueue_header *header;
	struct shmqueue_item *item;
	uint32_t hashidx;

	header = shmq_header->header;
	hashidx = shmq_header->hash_callback(key) % header->sha_hashnum;

	shmqueue_lock(shmq_header);
	for (item = RTAILQ_FIRST(&header->sha_hashlist[hashidx], shmqueue_item);
	    item != NULL;
	    item = RTAILQ_NEXT(item, &header->sha_hashlist[hashidx], shmqueue_item, shi_hashlist)) {

		if (SHMQUEUE_STRCMP(item->shi_keyvalue.kv_key, key) == 0) {
			/* found! */

			/*
			 * move to top of LRU.
			 * delete from LRU, and insert to top
			 */
			RTAILQ_REMOVE(&header->sha_lrulist,
			    item, shmqueue_item, shi_list);
			RTAILQ_INSERT_HEAD(&header->sha_lrulist,
			    item, shmqueue_item, shi_list);

			shmqueue_unlock(shmq_header);
			return &item->shi_keyvalue;
		}
	}
	shmqueue_unlock(shmq_header);
	return NULL;
}

struct keyvalue *
shmqueue_keyvalue_fetch(struct shmqueue *shmq_header, uint8_t *key, struct keyvalue *keyvalue)
{
	struct shmqueue_header *header;
	struct shmqueue_item *item;
	uint32_t hashidx;

	header = shmq_header->header;
	hashidx = shmq_header->hash_callback(key) % header->sha_hashnum;

	shmqueue_lock(shmq_header);
	for (item = RTAILQ_FIRST(&header->sha_hashlist[hashidx], shmqueue_item);
	    item != NULL;
	    item = RTAILQ_NEXT(item, &header->sha_hashlist[hashidx], shmqueue_item, shi_hashlist)) {

		if (SHMQUEUE_STRCMP(item->shi_keyvalue.kv_key, key) == 0) {
			/* found! */

			/*
			 * move to top of LRU.
			 * delete from LRU, and insert to top
			 */
			RTAILQ_REMOVE(&header->sha_lrulist,
			    item, shmqueue_item, shi_list);
			RTAILQ_INSERT_HEAD(&header->sha_lrulist,
			    item, shmqueue_item, shi_list);

			/* copy to caller */
			keyvalue->kv_storagesize = item->shi_keyvalue.kv_storagesize;
			SHMQUEUE_STRCPY(keyvalue->kv_key, item->shi_keyvalue.kv_key);
			memcpy(keyvalue->kv_storage, item->shi_keyvalue.kv_storage, keyvalue->kv_storagesize);

			shmqueue_unlock(shmq_header);
			return keyvalue;
		}
	}
	shmqueue_unlock(shmq_header);
	return NULL;
}

int
shmqueue_keyvalue_store(struct shmqueue *shmq_header, uint8_t *key, uint8_t *storage, uint32_t storage_size)
{
	struct shmqueue_header *header;
	struct shmqueue_item *item;
	uint32_t hashidx;

	header = shmq_header->header;

	if (storage_size >= SHMQUEUE_ITEM_STORAGESIZE)
		return -1;

	hashidx = shmq_header->hash_callback(key) % header->sha_hashnum;

	shmqueue_lock(shmq_header);
	for (item = RTAILQ_FIRST(&header->sha_hashlist[hashidx], shmqueue_item);
	    item != NULL;
	    item = RTAILQ_NEXT(item, &header->sha_hashlist[hashidx], shmqueue_item, shi_hashlist)) {

		if (SHMQUEUE_STRCMP(item->shi_keyvalue.kv_key, key) == 0) {
			/* already exists! */

			/*
			 * move to top of LRU.
			 * delete from LRU, and insert to top
			 */
			RTAILQ_REMOVE(&header->sha_lrulist,
			    item, shmqueue_item, shi_list);
			RTAILQ_INSERT_HEAD(&header->sha_lrulist,
			    item, shmqueue_item, shi_list);

			/* copy from caller */
			item->shi_keyvalue.kv_storagesize = storage_size;
			SHMQUEUE_STRCPY(item->shi_keyvalue.kv_key, key);
			memcpy(item->shi_keyvalue.kv_storage, storage, storage_size);

			shmqueue_unlock(shmq_header);
			return 0;
		}
	}
	/* not found */
	shmqueue_unlock(shmq_header);


	/* allocate pool */
	item = shmqueue_remove_from_freelist(shmq_header);
	if (item == NULL)
		return -1;

	/* update data, and store */
	strcpy((char *)item->shi_keyvalue.kv_key, (char *)key);
	item->shi_keyvalue.kv_storagesize = storage_size;
	memcpy(item->shi_keyvalue.kv_storage, storage, storage_size);
	item->shi_keyvalue.kv_storage[storage_size] = '\0';

	shmqueue_add_to_lru(shmq_header, item, hashidx);

	return 0;
}

int
shmqueue_keyvalue_delete(struct shmqueue *shmq_header, uint8_t *key)
{
	struct shmqueue_header *header;
	struct shmqueue_item *item;
	uint32_t hashidx;

	header = shmq_header->header;
	hashidx = shmq_header->hash_callback(key) % header->sha_hashnum;

	shmqueue_lock(shmq_header);
	for (item = RTAILQ_FIRST(&header->sha_hashlist[hashidx], shmqueue_item);
	    item != NULL;
	    item = RTAILQ_NEXT(item, &header->sha_hashlist[hashidx], shmqueue_item, shi_hashlist)) {

		if (SHMQUEUE_STRCMP(item->shi_keyvalue.kv_key, key) == 0) {
			/* found! */

			/* remove from LRU list */
			RTAILQ_REMOVE(&header->sha_lrulist,
			    item, shmqueue_item, shi_list);
			/* remove from HASH list */
			RTAILQ_REMOVE(&header->sha_hashlist[item->shi_hashidx],
			    item, shmqueue_item, shi_hashlist);

			header->sha_item_inuse--;

			/* insert to FREE list */
			RTAILQ_INSERT_HEAD(&header->sha_freelist,
			    item, shmqueue_item, shi_list);

			shmqueue_unlock(shmq_header);
			return 0;
		}
	}
	shmqueue_unlock(shmq_header);

	return -1;
}

int
shmqueue_inuse(struct shmqueue *shmq_header)
{
	int inuse;

	shmqueue_lock(shmq_header);
	inuse = shmq_header->header->sha_item_inuse;
	shmqueue_unlock(shmq_header);

	return inuse;
}

int
shmqueue_watermark(struct shmqueue *shmq_header)
{
	/*
	 * usage rate reaches 87.5%? (7/8)
	 */
	int mark = 0;

	shmqueue_lock(shmq_header);
	if (shmq_header->header->sha_itemnum -
	    (shmq_header->header->sha_itemnum / 8) <
	    shmq_header->header->sha_item_inuse)
		mark = 1;
	shmqueue_unlock(shmq_header);
	return mark;
}

int
shmqueue_getoldest(struct shmqueue *shmq_header, struct keyvalue *keyvalue)
{
	struct shmqueue_item *item;

	item = shmqueue_remove_from_lru(shmq_header);
	if (item == NULL)
		return -1;

	keyvalue->kv_storagesize = item->shi_keyvalue.kv_storagesize;
	SHMQUEUE_STRCPY(keyvalue->kv_key, item->shi_keyvalue.kv_key);
	memcpy(keyvalue->kv_storage, item->shi_keyvalue.kv_storage, keyvalue->kv_storagesize);
	keyvalue->kv_storage[keyvalue->kv_storagesize] = '\0';

	shmqueue_add_to_freelist(shmq_header, item);

	return 0;
}
