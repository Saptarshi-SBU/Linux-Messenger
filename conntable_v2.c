/* Connection pool based hashtable
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */
#include <linux/string.h>
#include <linux/cache.h>
#include <linux/net.h>
#include <linux/inet.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>

#include "conntable.h"
#include "stat.h"

#ifdef CONFIG_CACHEOBJS_CONNPOOL

/*
 * Ref : https://www.kfki.hu/~kadlec/sw/netfilter/ct3/
 *
 * We can probably replace this with murmash hash which takes lesser
 * cpu cyles. But could not find an existing kernel implementation.
 */
static inline u32 hashfn(__be32 daddr, __be32 port)
{
        static u32 hash_seed __read_mostly;

        net_get_random_once(&hash_seed, sizeof(hash_seed));
        return jhash_2words((__force u32) daddr, (__force u32) port, hash_seed);
}

/*
 * Convert ipv4 from literal to binary representation and compute hash
 * @ip   : (input)  ip address (parse will fail if ip is fed with hostnames)
 * @port : (input)  port
 * @*key : (output) contains computed hash value
 *
 * TBD   : perform ip conversion outside of core table operations
 */
static inline int ipv4_hash32(const unsigned char *ip, unsigned int port, u32 *key)
{
        __be32 daddr = 0;

        if (ip && (in4_pton(ip, strlen(ip), (u8 *)&daddr, '\0', NULL) == 1)) {
                *key = hashfn(daddr, (__be32) port);
                return 0;
        } else {
                pr_err("ipv4_hash32 error: null or invalid ip-tuple\n");
                return -EINVAL;
        }
}

/*
 * connection node reset stats
 */
static inline
void connection_node_reset_stats(struct cacheobj_connection_node *connp)
{
        cacheobjects_stat64_reset(&connp->nr_lookups);
        cacheobjects_stat64_reset(&connp->tot_js_get);
        cacheobjects_stat64_reset(&connp->tot_js_put);
        cacheobjects_stat64_reset(&connp->tot_js_wait);
        cacheobjects_stat64_reset(&connp->tx_bytes);
        cacheobjects_stat64_reset(&connp->rx_bytes);
}

/*
 * connection node stat for jiffies
 */
static inline
void connection_node_update_jiffies(struct cacheobj_connection_node *connp,
                conn_op_t op)
{
        switch (op) {
        case GET:
                cacheobjects_stat64_add(jiffies_now() - connp->now_js,
                        &connp->tot_js_get);
                break;
        case PUT:
                cacheobjects_stat64_add(jiffies_now() - connp->now_js,
                        &connp->tot_js_put);
                break;
        default:
                CONNTBL_ASSERT(0);
        }
}

/*
 * cacheobj_connection_node initialization
 */
inline int cacheobj_connection_node_init(struct cacheobj_connection_node *connp,
                const char *ip, unsigned int port)
{
        CONNTBL_ASSERT(ip);
        connp->ip = kstrdup(ip, GFP_KERNEL);
        if (!connp->ip) {
                pr_err("failed to allocate conn ip\n");
                return -ENOMEM;
        }
        connp->port = port;
        connp->state = CONN_DOWN;
        connp->flags = 0;
        connp->pool = NULL;
        connection_node_reset_stats(connp);
        return 0;
}

/*
 * check and release resources associated with cacheobj_connection_node.
 * TBD: Currently cacheobj_connection_node is an embedded structure and not
 * allocated, so free is not needed.
 * Note: If we reach here, that means we are good to die
 */
inline
int cacheobj_connection_node_destroy(struct cacheobj_connection_node *connp)
{
        CONNTBL_ASSERT(connp);
        CONNTBL_ASSERT(connp->pool);
        kfree(connp->ip);
        connp->pool = NULL; // uncache
        return 0;
}

/*
 * Move the connection to failed state
 */
inline
void cacheobj_connection_node_failed(struct cacheobj_connection_node *connp)
{
        // resource must be locked
        CONNTBL_ASSERT(test_and_set_bit_lock(CONN_LOCKED, &connp->flags));
        if ((connp->state == CONN_ACTIVE) || (connp->state == CONN_RETRY)) {
                clear_bit_unlock(CONN_LOCKED, &connp->flags);
                connp->state = CONN_FAILED;
        } else
                CONNTBL_ASSERT(0);
}

/*
 * Move the connection to retry state
 */
inline
void cacheobj_connection_node_retry(struct cacheobj_connection_node *connp)
{
        CONNTBL_ASSERT(!test_and_set_bit_lock(CONN_LOCKED, &connp->flags));
        connp->state = CONN_RETRY;
}

/*
 * Move the connection to ready state
 */
inline
void cacheobj_connection_node_ready(struct cacheobj_connection_node *connp)
{
        if (connp->state != CONN_RETRY)
                return;

        CONNTBL_ASSERT(test_and_set_bit_lock(CONN_LOCKED, &connp->flags));
        connp->state = CONN_READY;
        clear_bit_unlock(CONN_LOCKED, &connp->flags);
}

/*
 * initialize conn hash table and associated lock for protection
 * Note: We use a static hashtable(no resizing) for managing connection pools.
 */
static int connectionpool_hashtable_init(struct cacheobj_conntable *table)
{
        hash_init(table->buckets);
        rwlock_init(&table->lock);
        return 0;
}

/*
 * allocate and initialize a connection pool
 */
        static struct cacheobj_connection_pool *__connection_pool_alloc
(struct cacheobj_conntable *table, const char *ip, unsigned int port)
{
        int err = 0;
        struct cacheobj_connection_pool *pool;

        pool = (struct cacheobj_connection_pool *)
                kzalloc(sizeof(struct cacheobj_connection_pool), GFP_KERNEL);
        if (!pool) {
                pr_err("failed to allocate connection pool (%s:%u)\n", ip, port);
                err = -ENOMEM;
                goto poolmem_error;
        }

        pool->ip = kstrdup(ip, GFP_KERNEL);
        if (!pool->ip) {
                pr_err("failed to allocate pool ip (%s:%u)\n", ip, port);
                err = -ENOMEM;
                goto ipmem_error;
        }
        pool->port = port;

        // cache the hash key
        if (ipv4_hash32(pool->ip, pool->port, &pool->key) < 0) {
                err = -EINVAL;
                goto key_error;
        }

        INIT_LIST_HEAD(&pool->conn_list);
        INIT_HLIST_NODE(&pool->hentry);
        init_waitqueue_head(&pool->wq);

        atomic_set(&pool->upref, 0);
        atomic_set(&pool->nr_connections, 0);
        atomic_set(&pool->nr_idle_connections, 0);
        atomic64_set(&pool->nr_waits, 0);
        return pool;

key_error:
        kfree(pool->ip);

ipmem_error:
        kfree(pool);

poolmem_error:
        return ERR_PTR(-err);
}

/*
 * remove a connection pool.
 * notes:
 * -caller must have table write lock
 */
static int __connection_pool_destroy(struct cacheobj_connection_pool *pool)
{
        struct hlist_node *hentry;

        CONNTBL_ASSERT(pool);

        hentry = &pool->hentry;
        // pool must be in hash table!
        CONNTBL_ASSERT(hash_hashed(hentry));

        // note this is updated under a reader/writer lock
        // closes the timing window in which waiters are added to wq
        if (atomic_read(&pool->upref)) {
                pr_err("pool destroy error, pool has bumped up reference (%d)\n",
                                atomic_read(&pool->upref));
                goto pool_busy;
        }
        // cannot have pending waiters on pool's wait queue
        if (waitqueue_active(&pool->wq)) {
                pr_err("pool destroy error, pool has pending waiters\n");
                goto pool_busy;
        }

        // caller should ensure all connections dead/ready are removed from list
        if (!list_empty(&pool->conn_list)) {
                pr_err("pool destroy error, connection list is not empty\n");
                goto pool_busy;
        }

        CONNTBL_ASSERT(atomic_read(&pool->nr_connections) == 0);
        CONNTBL_ASSERT(atomic_read(&pool->nr_idle_connections) == 0);

        // upper layer must ensure no connections sneak in after this
        hash_del(hentry);
        pr_info("connection pool destroyed for <%s:%u>\n", pool->ip, pool->port);
        kfree(pool->ip);
        kfree(pool);
        return 0;

pool_busy:
        return -EBUSY;
}

/*
 * get connection pool given ip and port.
 * Note:
 * -caller must take reader lock to protect access
 * -pool is protected via:
 *   --rwlock
 *   --upref when not under rwlock(suspending on pool wait queue)
 */
static struct cacheobj_connection_pool *__get_connection_pool
        (struct cacheobj_conntable *table, const char *ip, unsigned int port)
{
        u32 key = 0;
        struct hlist_node *tmp;
        struct cacheobj_connection_pool *pool;

        if (ipv4_hash32(ip, port, &key) < 0)
                return ERR_PTR(-EINVAL);

        hash_for_each_possible_safe(table->buckets, pool, tmp, hentry, key) {
                if ((pool->port == port) && (strcmp(pool->ip, ip) == 0)) {
                        // probably insane check...
                        CONNTBL_ASSERT(pool->key == key);
                        return pool;
                }
        }
        return NULL;
}

/*
 * insert new connection entry to table, protected
 * returns 0 on success otherwise err
 */
static int connectionpool_hashtable_insert(struct cacheobj_conntable *table,
        struct cacheobj_connection_node *connp)
{
        struct cacheobj_connection_pool *pool;

        CONNTBL_ASSERT(connp);

        write_lock(&table->lock);
        pool = __get_connection_pool(table, connp->ip, connp->port);
        if (!pool) {
                write_unlock(&table->lock);
                pool = __connection_pool_alloc(table, connp->ip, connp->port);
                if (IS_ERR(pool)) {
                        pr_err("pool allocation failure\n");
                        return -ENOMEM;
                }

                write_lock(&table->lock);
                hash_add(table->buckets, &pool->hentry, pool->key);
        }
        CONNTBL_ASSERT(!IS_ERR(pool));
        connp->pool = pool;

        /* added to head of per-pool connection chain */
        list_add(&connp->list_node, &pool->conn_list);
        atomic_inc(&pool->nr_connections);

        connp->state = CONN_READY;
        atomic_inc(&pool->nr_idle_connections);

        atomic_inc(&pool->upref);
        write_unlock(&table->lock);

        // wakeup any pending waiters, preceding code has implicit barrier
        if (waitqueue_active(&pool->wq))
                wake_up_interruptible(&pool->wq);

        //pr_info("added new connection pool <%s:%u>", pool->ip, pool->port);
        atomic_dec(&pool->upref);
        return 0;
}

/*
 * remove helper, no lock version
 * note: caller must have table write lock
 */
static inline int __connection_remove(struct cacheobj_conntable *table,
        struct cacheobj_connection_node *connp)
{
        int err;
        struct cacheobj_connection_pool *pool;

        // we bail out if node is in use
        if (test_and_set_bit_lock(CONN_LOCKED, &connp->flags)) {
                err = -EBUSY;
                pr_err("conn is locked, cannot destroy!!!\n");
                goto remove_error;
        }
        // unlink from chain and update pool counters
        pool = connp->pool;
        CONNTBL_ASSERT(pool);
        CONNTBL_ASSERT(connp->state != CONN_ACTIVE);
        if (connp->state == CONN_READY) {
                atomic_dec(&pool->nr_idle_connections);
                connp->state = CONN_ZOMBIE;
        }

        list_del(&connp->list_node);
        atomic_dec(&pool->nr_connections);
        return 0;

remove_error:
        pr_err("failed to remove connection (%s:%u)\n", connp->ip, connp->port);
        return err;
}

/*
 * remove connection entry from table, protected
 * returns 0 on success otherwise -EBUSY on error
 */
static int connectionpool_hashtable_remove(struct cacheobj_conntable
	*table, struct cacheobj_connection_node *connp)
{
        int err;

        write_lock(&table->lock);
        err = __connection_remove(table, connp);
        write_unlock(&table->lock);
        return err;
}

/*
 * looks up a connection entry from pool, protected
 * note: return node has no ownership and later validity cannot be assured.
 */
static struct cacheobj_connection_node *connectionpool_hashtable_peek
        (struct cacheobj_conntable *table, const char *ip, unsigned int port)
{
        struct cacheobj_connection_pool *pool;
        struct cacheobj_connection_node *conn_nodep = NULL;

        read_lock(&table->lock);

        pool = __get_connection_pool(table, ip, port);
        if (pool && !IS_ERR(pool) && !list_empty(&pool->conn_list)) {
                conn_nodep = list_first_entry(&pool->conn_list,
                                struct cacheobj_connection_node, list_node);
        }

        read_unlock(&table->lock);
        return conn_nodep;
}

/*
 * iterator function for conntable, protected
 * note returned connection handle is not be locked
 */
static struct cacheobj_connection_node *connectionpool_hashtable_iter
        (struct cacheobj_conntable *table)
{
        int bkt = 0;
        struct cacheobj_connection_pool *pool;
        struct cacheobj_connection_node *connp, *tmp;

        read_lock(&table->lock);
        hash_for_each(table->buckets, bkt, pool, hentry) {
                list_for_each_entry_safe(connp, tmp, &pool->conn_list, list_node) {
                        read_unlock(&table->lock);
                        return connp;
                }
        }
        read_unlock(&table->lock);
        return NULL;
}

/*
 * gets a ready and exclusive connection from pool conn list, no lock version
 * returns :
 * 	 locked cacheobj_connection_node on success
 *	 NULL on no entry
 *	-EINVAL on bad input
 *	-EBUSY on resource busy
 *	-EPIPE on all paths down
 * notes : caller must ensure we have table read lock
 */
static struct cacheobj_connection_node* connection_get
        (struct cacheobj_connection_pool *pool, unsigned long now_js)
{
        int err = 0;
        bool apd = true;
        struct cacheobj_connection_node *connp, *tmp;

        list_for_each_entry_safe(connp, tmp, &pool->conn_list, list_node) {
                if (test_and_set_bit_lock(CONN_LOCKED, &connp->flags)) {
                        apd = false; // hint we did not check the state
                        continue;
                }
                // got ownership
                if (connp->state == CONN_READY) {
                        atomic_dec(&connp->pool->nr_idle_connections);
                        connp->state = CONN_ACTIVE;
                        // end wait time
                        cacheobjects_stat64_add(jiffies_now() - now_js,
                                        &connp->tot_js_wait);
                        // start use time
                        cacheobjects_stat64_jiffies(&connp->now_js);
                        cacheobjects_stat64(&connp->nr_lookups);
                        return connp;
                } else {
                        clear_bit_unlock(CONN_LOCKED, &connp->flags);
                }
        }

        // error path:
        if (list_empty(&pool->conn_list)) {
                pr_debug("get connection node error <%s:%u>, node not present "
                                "in pool", pool->ip, pool->port);
                err = -ENOENT;
        } else if (apd) {
                pr_debug("get connection node failed <%s:%u>, all paths down "
                                "to node!", pool->ip, pool->port);
                err = -EPIPE;
        } else {
                pr_debug("get connection node error <%s:%u>, resource busy!",
                                pool->ip, pool->port);
                err = -EBUSY;
        }
        return ERR_PTR(err);
}

/*
 * get a ready and exclusive connection with timeout.
 * -may suspend current task with timeout if pool is busy
 * returns:
 *	locked connection on success
 *	ERR_PTR or NULL on error
 */
static struct cacheobj_connection_node* connection_timed_get
        (struct cacheobj_conntable *table, const char *ip, unsigned int port,
        long timeout)
{
        unsigned long now_js;
        struct cacheobj_connection_node *connp = NULL;
        struct cacheobj_connection_pool *pool;

        // start wait time
        cacheobjects_stat64_jiffies(&now_js);

        do {
                read_lock(&table->lock);

                pool = __get_connection_pool(table, ip, port);
                if (!pool) {
                        pr_err("get failed, pool not initialized (%s:%u)\n",
                                        ip, port);
                        goto exit;
                }
                CONNTBL_ASSERT(!IS_ERR(pool));
                connp = connection_get(pool, now_js);
                // got one!
                if (likely(!IS_ERR(connp))) {
			read_unlock(&table->lock);
			return connp;
		}

                switch(PTR_ERR(connp)) {
                case -ENOENT:
                        // pool empty
                        connp = NULL;
                case -EPIPE: // deliberate fall through
                        // apd
                        CONNTBL_ASSERT
                                (atomic_read(&pool->nr_idle_connections) == 0);
                        goto exit;
                case -EBUSY:
                        // resource busy
                        // It is important that we hold lock when doing upref otherwise,
                        // writer can nuke a pool, while we try to upref the same
                        atomic_inc(&pool->upref);
                        read_unlock(&table->lock);

                        atomic64_inc(&pool->nr_waits);
                        // TBD : check for shutdown in progress
                        timeout = wait_event_interruptible_timeout(pool->wq, 
                                (atomic_read(&pool->nr_idle_connections) > 0),
                                timeout);
                        atomic_dec(&pool->upref);
                        break;
                default:
                        CONNTBL_ASSERT(0);
                }
                //pr_info("timeout jiffies remaining :%ld\n", timeout);
        } while (timeout >= 0);

        CONNTBL_ASSERT(timeout == 0);
        if (!connp || IS_ERR(connp))
                pr_err("get connection timed out<%s:%u>\n", ip, port);

        return connp;

exit:
        read_unlock(&table->lock);
        return connp;
}

/*
 * puts a connection after use
 * -unlock connection and notify one waiting on pool wq
 */
static void connection_put(struct cacheobj_conntable *table,
        struct cacheobj_connection_node *connp, conn_op_t op)
{
        switch (connp->state) {
        case CONN_ACTIVE: {
                        struct cacheobj_connection_pool *pool = connp->pool;

                        // We perform steps in reverse that we do to lock
                        // 1. change state to ready
                        // 2. bump pool reference
                        // 3. impose barrier to ensure stores are not reordered
                        // 4. release connection lock
                        // 5. wake up waiters on pool
                        // 6. release pool reference

                        // end use time
                        connection_node_update_jiffies(connp, op);
                        connp->state = CONN_READY;
                        atomic_inc(&pool->upref);
                        atomic_inc(&pool->nr_idle_connections);
                        clear_bit_unlock(CONN_LOCKED, &connp->flags);
                        if (waitqueue_active(&pool->wq))
                                wake_up_interruptible(&pool->wq); // wake up a single task
                        atomic_dec(&pool->upref);
                        break;
                }
        default:
                        clear_bit_unlock(CONN_LOCKED, &connp->flags);
                        break;
        }
}

/*
 * clears connection table, protected
 */
static int connectionpool_hashtable_destroy(struct cacheobj_conntable *table)
{
        int err = 0, bkt;
        size_t nr_items = 0;
        struct hlist_node *tmp;
        struct cacheobj_connection_pool *pool;
        struct cacheobj_connection_node *connp, *tmp_list;

        write_lock(&table->lock);
        if (hash_empty(table->buckets))
                goto exit;

        hash_for_each_safe(table->buckets, bkt, tmp, pool, hentry) {
                // iterate connection pool
                list_for_each_entry_safe(connp, tmp_list, &pool->conn_list,
                        list_node) {
                        err = __connection_remove(table, connp);
                        if (err) {
                                pr_err("connection remove error <%s:%u>\n",
                                        connp->ip, connp->port);
                                goto next_pool;
                        }
                        (void) cacheobj_connection_node_destroy(connp);
                        nr_items++;
                }
                // pool not ready to destroy
                if (__connection_pool_destroy(pool) < 0)
                        pr_err("failed to destroy pool (%s:%u)\n", pool->ip,
                                pool->port);

next_pool:
                continue;
        }
exit:
        write_unlock(&table->lock);
        pr_info("cleanup removed %lu items from table\n", nr_items);
        return err;
}

/*
 * track cacheobj_connection_node usage distribution
 */
static void connectionpool_hashtable_dump(struct cacheobj_conntable
	*table, struct seq_file *m)
{
        int bkt;
        struct hlist_node *tmp;
        unsigned long total, getus, putus, wtus;
        u64 lookups, waits, tx_mb, rx_mb;
        struct cacheobj_connection_pool *pool;
        struct cacheobj_connection_node *connp, *tmp_list;

        seq_printf(m, "HOST\tSTATE\tRETRIES\tLOOKUPS\tWAITS\tAVG_WAIT(us)\t"
                        "AVG_LAT_GET(us)\tAVG_LAT_PUT(us)\tSEND(kb) RCV(kb)\n");

        read_lock(&table->lock);

        if (hash_empty(table->buckets))
                goto exit;

        hash_for_each_safe(table->buckets, bkt, tmp, pool, hentry) {
                //pr_info("pool : (%s:%u) nr_waits :%lu", pool->ip,
                //        pool->port, atomic64_read(&pool->nr_waits));
                list_for_each_entry_safe(connp, tmp_list, &pool->conn_list,
                                list_node) {
                        lookups = atomic64_read(&connp->nr_lookups);
                        waits = cacheobjects_stat64_read(&connp->nr_waits);
                        tx_mb = cacheobjects_stat64_read(&connp->tx_bytes)
                                >> 10;
                        rx_mb = cacheobjects_stat64_read(&connp->rx_bytes)
                                >> 10;
                        total = cacheobjects_stat64_jiffies2usec
                                (&connp->tot_js_get);
                        getus = div64_safe(total, lookups);
                        total = cacheobjects_stat64_jiffies2usec
                                (&connp->tot_js_put);
                        putus = div64_safe(total, lookups);
                        total = cacheobjects_stat64_jiffies2usec
                                (&connp->tot_js_wait);
                        wtus = div64_safe(total, lookups);
                        seq_printf(m, "%s:%u %s %u %llu %llu %lu %lu %lu %llu "
                                "%llu\n", connp->ip, connp->port,
                                conn_state_status(connp->state),
                                connp->nr_retry_attempts, lookups, waits, wtus,
                                getus, putus, tx_mb, rx_mb);
                }
        }
exit:
        read_unlock(&table->lock);
}

const struct cacheobj_conntable_operations cacheobj_conntable_ops =
{
        .cacheobj_conntable_init = connectionpool_hashtable_init,
        .cacheobj_conntable_destroy = connectionpool_hashtable_destroy,
        .cacheobj_conntable_insert = connectionpool_hashtable_insert,
        .cacheobj_conntable_remove = connectionpool_hashtable_remove,
        .cacheobj_conntable_peek = connectionpool_hashtable_peek,
        .cacheobj_conntable_iter = connectionpool_hashtable_iter,
        .cacheobj_conntable_timed_get = connection_timed_get,
        .cacheobj_conntable_put = connection_put,
        .cacheobj_conntable_dump = connectionpool_hashtable_dump
};

#endif
