# MySQL / InnoDB Storage Engine

> An overview of how **InnoDB**, MySQL's default storage engine, organizes, manages, and safeguards data. This guide explores **clustered indexes**, **secondary indexes**, the **buffer pool**, **undo** and **redo logs**, **row-level locking** with **gap locks**, and **Oracle-style MVCC**. Along the way, InnoDB's architecture is compared with PostgreSQL's append-only heap and **VACUUM** approach, with each behavioral observation supported by experiments performed on a live **MySQL 8.0.46 / InnoDB** instance.

---

## 1. Problem Background

InnoDB was introduced to provide MySQL with a storage engine capable of supporting **transactions, crash recovery, and high concurrency**, addressing the shortcomings of MyISAM, which relied on table-level locking and lacked recovery after failures. Internally, InnoDB follows an **ARIES-inspired** design that performs **in-place updates**, using **undo logs** to reverse changes or recreate earlier row versions and **redo logs** to restore committed updates after a crash.

The remainder of InnoDB's architecture is largely shaped by two fundamental design choices:

1. **A table is physically stored as its primary-key B-tree (clustered index).** Rather than storing rows separately, the leaf nodes of the primary-key index contain the actual row data in primary-key order. This organization makes primary-key lookups and range scans especially efficient.

2. **Multi-Version Concurrency Control (MVCC) is implemented through undo logs.** Instead of storing multiple row versions directly inside the table, InnoDB reconstructs previous versions from the undo log whenever they are required. This minimizes table bloat, although reading older snapshots may require traversing a chain of undo records.

These two architectural decisions are also the primary areas where InnoDB differs from PostgreSQL, making that comparison a recurring theme throughout this document.

---

## 2. Architecture Overview

The executor interacts exclusively with the **buffer pool**, meaning data modifications are performed on in-memory pages rather than directly on disk. Before any row is updated, its previous state is written to the **undo log**. The modification itself is then recorded in the **redo log** before any dirty page is flushed to persistent storage, following the write-ahead logging principle.

Dirty pages are written back to the tablespace asynchronously by **page cleaner** threads, while **purge** threads remove obsolete undo records once no active transaction requires them for rollback or snapshot reads.

---

## 3. Internal Design

### 3.1 Clustered Index & Primary-Key Storage

In InnoDB, the **primary key itself forms a B+-tree whose leaf pages store complete table rows**. Unlike systems that separate heap storage from indexes, there is no independent heap structure—the clustered index is the table.

This design has several important implications:

* A **primary-key lookup** requires traversing only a single B-tree because the requested row is already located in the leaf page.
* **Range scans on the primary key** are efficient since rows with nearby key values are stored physically close together.
* The choice of primary key significantly affects performance. Sequential keys such as `AUTO_INCREMENT` naturally append to the rightmost leaf page, whereas random values such as UUIDs distribute inserts across the tree, increasing page splits and fragmentation. If no primary key is defined, InnoDB automatically generates a hidden 6-byte `DB_ROW_ID` to serve this purpose.

A useful way to contrast the two database systems is:

* **PostgreSQL:** rows are stored in a heap, while the primary-key index points to them.
* **InnoDB:** the primary-key index itself contains the rows.

---

### 3.2 Secondary Indexes

Secondary indexes are implemented as independent B-trees whose leaf nodes store the **indexed column values together with the corresponding primary key**, rather than a physical pointer to the row.

As a result, retrieving a row through a secondary index generally involves two lookup operations:

```text
secondary index
        ↓
primary key value
        ↓
clustered index
        ↓
row
```

This additional traversal, often called a **bookmark lookup**, disappears when the secondary index is **covering**. In that case, every column required by the query is already available within the index, allowing InnoDB to satisfy the request without accessing the clustered index.

This behavior is demonstrated in **Experiment A**, where a standard secondary-index lookup has an estimated cost of **1.4**, while the covering-index version is reduced to **0.651**. The improvement comes entirely from eliminating the extra clustered-index lookup.

Using the primary key instead of a physical row pointer is also an intentional design decision. If rows are relocated because of page splits or reorganization within the clustered index, secondary indexes remain valid since they reference the logical primary key rather than a physical storage location.

---

## 3.3 Buffer Pool

The **buffer pool** is InnoDB's primary memory cache and stores database pages of **16 KB** each (configured as **128 MB** by default in the live system). Rather than reading from or writing directly to disk for every operation, InnoDB serves most requests through this in-memory cache, greatly reducing disk I/O.

To decide which pages remain in memory, InnoDB uses a **midpoint-insertion LRU** algorithm instead of a traditional Least Recently Used (LRU) list. The cache is divided into two regions:

* A **young** sublist that contains frequently accessed pages.
* An **old** sublist that temporarily holds pages that have been read recently.

When a page is loaded from disk, it is inserted into the **middle** of the LRU list instead of immediately being treated as a hot page. This prevents a large one-time scan—such as a full table scan—from displacing pages that are repeatedly accessed by normal workloads. Pages involved in the scan remain in the *old* portion of the list and are removed relatively quickly unless they are accessed again.

Beyond caching data pages, the buffer pool also supports **change buffering**, allowing updates to secondary indexes to be deferred when the relevant index pages are not already resident in memory. Additionally, it maintains the **Adaptive Hash Index**, which can accelerate repeated lookups by creating hash-based access paths for frequently used index searches.

---

### 3.4 Redo Log

The **redo log** serves as InnoDB's implementation of **Write-Ahead Logging (WAL)**. It is a fixed-size, circular log that records **physical page modifications** before any changed data pages are written back to disk. In the live configuration examined here, the redo log capacity (`innodb_redo_log_capacity`) is **100 MB**.

Its operation follows a straightforward sequence:

* Whenever a transaction modifies a page, the corresponding change is first written to the **redo log**, while the affected page in the buffer pool is marked as dirty.
* Only after the redo record has been safely persisted can the modified page be flushed from memory to the tablespace. With `innodb_flush_log_at_trx_commit = 1` (verified in the live system), the redo log is `fsync`ed on every transaction commit, providing the highest level of durability.
* If the database crashes before dirty pages reach disk, recovery simply replays the committed redo records, restoring the database to its most recent consistent state.

One of the primary advantages of the redo log is that it converts expensive **random disk writes** into efficient **sequential log writes**. Instead of forcing every modified data page to be written immediately, InnoDB records the change sequentially in the redo log and postpones flushing the actual pages until background processes handle them. This significantly improves write performance while still ensuring durability.

---

## 3.5 Undo Log & MVCC

The **undo log** stores the previous version of every row modified by a transaction. These records are maintained in dedicated undo tablespaces (such as `innodb_undo_001` and `innodb_undo_002`, confirmed in the live system) and support two essential database functions.

The first is **transaction rollback**. If a transaction must be aborted, InnoDB processes the corresponding undo records in reverse order, restoring each row to its earlier state and effectively reversing all changes made by that transaction.

The second is **Multi-Version Concurrency Control (MVCC)**. Rather than keeping multiple row versions inside the table, InnoDB reconstructs historical versions from the undo log whenever a transaction needs to read an older snapshot. Each row contains hidden metadata—including `DB_TRX_ID`, which records the transaction that last modified the row, and `DB_ROLL_PTR`, which references the associated undo record. When a consistent read encounters a row that is newer than the reader's snapshot, InnoDB follows the undo chain backwards until it reaches a version that is visible to that transaction.

This design represents one of the most significant architectural differences between InnoDB and PostgreSQL. PostgreSQL stores every row version directly within the table and relies on **VACUUM** to eventually remove obsolete tuples. In contrast, InnoDB stores only the most recent version of each row in the clustered index, reconstructing older versions from the undo log whenever they are required. As a result, InnoDB avoids accumulating dead row versions inside the table itself. The trade-off is that transactions holding old snapshots can force the system to maintain long undo chains, increasing the history list until purge threads are able to remove obsolete records.

**Experiment G** illustrates this behavior. One transaction initially read `bal = 100`. While that transaction remained active, another transaction updated the balance to `999` and committed. A second read performed within the original transaction still returned `100`, demonstrating that InnoDB reconstructed the earlier version from the undo log in order to preserve the transaction's consistent snapshot.

--- 
## 3.6 Locking: Row, Gap & Next-Key

InnoDB provides **row-level locking**, allowing transactions that modify different rows to execute concurrently with minimal interference. Rather than maintaining a centralized lock table, lock information is associated with the corresponding index records, enabling fine-grained concurrency and reducing unnecessary contention.

This behavior is demonstrated in **Experiment E**. Transaction B was able to update a different row immediately while Transaction A held a lock on another row. However, when Transaction B attempted to modify the same row that Transaction A had already locked, it was forced to wait until Transaction A committed. The operation completed successfully after the lock was released, illustrating that conflicting writes block rather than fail.

To prevent **phantom reads** under the default **REPEATABLE READ** isolation level, InnoDB extends row locking by also protecting the spaces between index records. This is achieved through three related lock types:

* **Record lock** – Locks a specific index record, preventing concurrent modifications to that row.
* **Gap lock** – Locks the interval between adjacent index records, preventing new rows from being inserted into that range.
* **Next-key lock** – Combines a record lock with the gap immediately preceding it. This is the locking strategy InnoDB normally applies during range scans.

These additional lock types ensure that the set of rows visible to a transaction remains stable throughout its execution.

**Experiment F** highlights this behavior. A transaction executed a `SELECT ... FOR UPDATE` on the range `(10, 20)`, acquiring next-key locks over the scanned interval. While those locks were held, another transaction attempted to insert a row with `id = 15`. Since the value fell inside the protected gap, the insert was blocked and eventually timed out. An insert using `id = 99`, which lay outside the locked range, completed immediately. By preventing inserts into the protected interval, gap and next-key locks eliminate phantom rows during locking reads under **REPEATABLE READ**, providing stronger guarantees than those required by the SQL standard.

---

## 3.7 Isolation Levels

InnoDB uses **REPEATABLE READ** as its default transaction isolation level, which is notably stronger than PostgreSQL's default of **READ COMMITTED**. This choice has a significant impact on how concurrent transactions observe changes made by one another.

Under **REPEATABLE READ**, a transaction establishes a consistent snapshot the first time it performs a non-locking read. Every subsequent read within that transaction refers back to the same snapshot, even if other transactions commit newer versions of the same rows in the meantime. **Experiment G** demonstrates this behavior, where repeated reads returned the original value despite another transaction committing an update between them.

For locking operations such as `SELECT ... FOR UPDATE` and `SELECT ... LOCK IN SHARE MODE`, InnoDB supplements snapshot isolation with **gap** and **next-key locks**. These locks prevent new rows from appearing within locked ranges, ensuring that phantom rows cannot occur during range-based locking reads.

Like most relational database systems, InnoDB supports all four standard SQL isolation levels:

* `READ UNCOMMITTED`
* `READ COMMITTED`
* `REPEATABLE READ`
* `SERIALIZABLE`

Although all four levels are available, the default **REPEATABLE READ** configuration provides stronger consistency guarantees than many developers expect, particularly because of InnoDB's use of MVCC together with gap and next-key locking.

---

# 4. InnoDB vs PostgreSQL — The MVCC Fork in the Road

| Dimension                | PostgreSQL                               | InnoDB                                             |
| ------------------------ | ---------------------------------------- | -------------------------------------------------- |
| Table physical form      | **Heap** (unordered) + separate PK index | **Clustered index** (rows stored in the PK B-tree) |
| Update strategy          | **Append new tuple version**             | **In-place update** with undo logging              |
| Location of old versions | Stored directly in the table             | Stored in the **undo log**                         |
| Garbage collection       | **VACUUM** removes obsolete tuples       | **Purge** threads remove obsolete undo records     |
| Secondary index entries  | Reference heap `ctid`                    | Reference the **primary key**                      |
| Logging                  | WAL (redo only)                          | Separate **redo** and **undo** logs                |
| Default isolation        | READ COMMITTED                           | **REPEATABLE READ**                                |
| Phantom prevention       | Predicate locks at SERIALIZABLE          | **Gap and next-key locks** at REPEATABLE READ      |

Although both PostgreSQL and InnoDB implement **MVCC**, they achieve it using fundamentally different storage strategies.

InnoDB performs **in-place updates**, meaning that modifying a row overwrites its current version. Because the previous version would otherwise be lost, InnoDB records it in the **undo log**. Those undo records serve two purposes: they allow transactions to roll back incomplete changes, and they provide historical row versions for transactions reading older snapshots. Since in-place updates also risk losing committed changes during a crash, InnoDB maintains a separate **redo log** that enables recovery by replaying committed modifications.

PostgreSQL follows a different approach. Rather than overwriting existing rows, every update creates a new tuple version while leaving older versions in the table. Rollback simply ignores uncommitted tuples, and readers locate the version appropriate for their snapshot without consulting a separate undo structure. Once older versions are no longer needed, **VACUUM** reclaims the storage.

As a result, both database systems deliver the same guarantees—**atomicity, durability, and multi-version concurrency control**—but they achieve those guarantees by storing historical row versions in completely different places. That single architectural decision explains many of the practical differences between the two engines, including their logging mechanisms, maintenance processes, and storage behavior.

--- 

## 5. Design Trade-Offs

Every storage engine makes architectural compromises, and InnoDB is no exception. Many of its strengths arise directly from its core design decisions, but those same choices introduce trade-offs that influence performance, storage requirements, and application behavior.

### Advantages of InnoDB

* **Clustered storage** enables highly efficient primary-key lookups because the data itself resides within the primary-key B-tree. It also makes primary-key range scans naturally sequential, improving locality and reducing disk access.
* **In-place updates combined with undo-based MVCC** keep tables compact by storing only the latest version of each row in the clustered index. Readers can still access consistent historical snapshots without blocking concurrent writers.
* **Fine-grained row locking**, together with gap and next-key locks, allows many transactions to modify different rows simultaneously while still preventing phantom rows under the default **REPEATABLE READ** isolation level.

### Costs and Limitations

* Accessing data through a **secondary index** usually requires an additional lookup. The secondary index first identifies the primary key, after which InnoDB must traverse the clustered index to retrieve the complete row. Only covering indexes eliminate this extra step.
* Because every secondary index stores a copy of the **primary key**, choosing a wide primary key increases the storage overhead of every secondary index in the table.
* Transactions that remain open for long periods delay the removal of obsolete undo records. As undo chains grow longer, reconstructing historical row versions becomes more expensive, and purge threads must process a larger history list before reclaiming space.
* Primary-key design has a direct impact on storage efficiency. Sequential values such as `AUTO_INCREMENT` encourage append-style inserts, whereas random keys frequently trigger page splits and fragmentation throughout the clustered index.
* Gap locks, while necessary for phantom prevention, can sometimes produce unexpected blocking behavior. Applications may observe inserts waiting on transactions that appear to be modifying different rows, increasing the likelihood of lock contention or deadlocks if developers are unfamiliar with InnoDB's locking model.

PostgreSQL makes many of the opposite trade-offs. Its heap-based storage avoids the secondary-index double lookup and does not duplicate the primary key in every index. However, storing multiple tuple versions directly inside the table introduces table and index bloat over time, making regular **VACUUM** operations essential for maintaining performance.

---

# 6. Experiments / Observations

> **Experimental Setup.** All experiments were performed using **MySQL 8.0.46** with the InnoDB storage engine (Docker image `mysql:8.0`). The test dataset consisted of **50,000 customers**, **200,000 orders**, and **600,000 order_items**, with primary keys and appropriate secondary indexes defined on foreign-key columns. Every result shown below was collected from actual executions.

---

### Experiment A — Clustered vs Secondary vs Covering Index

```
-- PK lookup (clustered index, no indirection):
EXPLAIN: -> Rows fetched before execution  (cost=0..0 rows=1)

-- Secondary-index lookup (then back to clustered index for the row):
EXPLAIN: -> Index lookup on orders using idx_customer (customer_id=12345)  (cost=1.4 rows=4)

-- Covering secondary index (answer entirely from the index, NO clustered probe):
EXPLAIN: -> Covering index lookup on orders using idx_customer (customer_id=12345)  (cost=0.651 rows=4)
```

**Observation.**

The optimizer's estimated costs clearly demonstrate how clustered storage affects lookup performance. A covering index lookup has a lower estimated cost (**0.651**) than a standard secondary-index lookup (**1.4**) because the required columns are already present in the secondary index. Eliminating the additional traversal from the secondary index to the clustered index removes the extra work normally required to retrieve the complete row.

---

### Experiment B — Buffer Pool

```
 buffer_pool_MB | page_bytes
----------------+-----------
       128      |   16384

 pages_total | FREE_BUFFERS | data_pages | hit_rate_pct
-------------+--------------+------------+-------------
        8192 |         2269 |       5505 |        100.0
```

**Observation.**

With a buffer pool size of **128 MB** and a page size of **16 KB**, the system provides space for **8,192 pages**. Following the workload, **5,505 pages** contained cached data, while the measured **100% hit rate** indicates that the active working set resided entirely in memory. Consequently, subsequent reads were satisfied directly from the buffer pool instead of requiring disk access.

---

### Experiment C — Redo & Undo Configuration

```
 redo_capacity_MB | flush_at_commit
------------------+----------------
       100        |       1            <- fsync redo on every commit (durable)

 undo tablespaces:  innodb_undo_001 (16 MB),  innodb_undo_002 (32 MB)
```

**Observation.**

The experiment confirms that InnoDB maintains **redo** and **undo** information as separate persistent structures. The configuration `innodb_flush_log_at_trx_commit = 1` ensures that redo records are flushed to stable storage on every commit, providing full durability. Meanwhile, the dedicated undo tablespaces store rollback information and historical row versions used by MVCC.

---

### Experiment D — Three-Table Join Plan

```
-> Group aggregate  (cost=42017 rows=4)
  -> Nested loop inner join  (cost=36123 rows=58944)
    -> Nested loop inner join  (cost=15492 rows=19740)
      -> Covering index lookup on c using idx_country (country='IN')  (cost=1004 rows=10000)
      -> Index lookup on o using idx_customer (customer_id=c.id)      (cost=1.03 rows=4.14)
    -> Index lookup on oi using idx_order (order_id=o.id)             (cost=0.747 rows=2.99)
```

**Observation.**

The optimizer selected an **index nested-loop join** strategy for this query. Execution begins with the relatively small set of customers matching `country = 'IN'`, obtained through a covering index. The resulting rows are then used to probe the `orders` table, followed by lookups into `order_items`, both through secondary indexes.

This execution strategy contrasts with PostgreSQL's plan for the equivalent query, where a **parallel hash join** was chosen instead. The comparison illustrates a broader difference in optimizer philosophy: MySQL generally favors index-driven nested-loop joins and, in this scenario, does not parallelize the execution of a single query.

---

### Experiment E — Row-Level Locking

```
[A holds an open UPDATE on row id=1]
[t=1s] B updates row id=2  -> "B updated row 2 WITHOUT waiting"
[t=1s] B updates row id=1  -> WAITS ~2.85s -> "B updated row 1 AFTER A committed"
final: id1=95, id2=80   (both applied correctly)
```

**Observation.**

The experiment demonstrates that InnoDB locks individual rows rather than entire tables. While Transaction A held a lock on one row, Transaction B successfully updated a different row without delay. Only when Transaction B attempted to modify the already locked row was it forced to wait until Transaction A committed. This behavior confirms that write conflicts are resolved through waiting rather than immediate failure, while unrelated rows remain fully accessible to concurrent transactions.

---

### Experiment F — Gap Lock (Phantom Prevention)

```
[A holds: SELECT id FROM acct WHERE id BETWEEN 10 AND 20 FOR UPDATE]   (gap 10..20 locked)
[t=1s] B INSERT id=15 (inside gap)  -> ERROR 1205: Lock wait timeout exceeded   (BLOCKED)
[t=1s] B INSERT id=99 (outside gap) -> "inserted id=99 (no wait)"               (ALLOWED)
```

**Observation.**

This experiment demonstrates how InnoDB prevents phantom rows during locking range queries. When Transaction A executed `SELECT ... FOR UPDATE` over the range `(10, 20)`, InnoDB acquired **next-key locks**, protecting not only the existing rows but also the gaps between them.

As a result, Transaction B was unable to insert a new row with `id = 15` because the value fell within the locked interval. The operation remained blocked until it eventually timed out. In contrast, inserting `id = 99`, which lay outside the protected range, completed immediately without waiting.

This behavior illustrates the role of gap and next-key locks in maintaining **phantom-free REPEATABLE READ**. By preventing inserts into the locked range, InnoDB guarantees that repeated range queries within the same transaction continue to observe a consistent set of rows.

---

### Experiment G — MVCC Consistent Read (Undo Log in Action)

```
[A] first read  bal of id=30  -> 100
[t=1s] B: UPDATE id=30 SET bal=999; COMMIT   ("B set bal=999")
[A] second read bal of id=30  -> 100         <- SAME snapshot, reconstructed from undo
```

**Observation.**

This experiment provides a direct demonstration of InnoDB's MVCC implementation. Transaction A initially read a balance of **100** for a particular row. While Transaction A remained active, Transaction B updated that row to **999** and committed the change.

Despite the committed update, Transaction A's second read still returned **100**. Rather than exposing the latest committed value, InnoDB reconstructed the earlier version from the **undo log**, ensuring that Transaction A continued reading from the same consistent snapshot established at the beginning of the transaction.

The result clearly illustrates how undo records support snapshot isolation without requiring multiple row versions to be stored directly within the table.

---

# 7. Key Learnings

1. **Understanding clustered storage is key to understanding InnoDB.** Because table rows are stored directly within the primary-key B-tree, primary-key lookups and range scans are highly efficient. The same design, however, requires secondary indexes to reference the primary key, introducing an additional lookup unless a covering index can satisfy the query. The cost difference observed in **Experiment A** illustrates this trade-off.

2. **Redo and undo logs perform distinct responsibilities.** Although both contribute to transaction processing, they solve different problems. The **redo log** guarantees durability by allowing committed changes to be replayed after a crash, whereas the **undo log** enables transaction rollback and reconstructs historical row versions required for MVCC. Since InnoDB updates rows in place, both logging mechanisms are essential.

3. **InnoDB and PostgreSQL implement MVCC using fundamentally different storage models.** InnoDB stores only the latest row version within the table and reconstructs older versions from the undo log. PostgreSQL, on the other hand, keeps multiple tuple versions directly inside the heap. This single architectural difference explains why InnoDB relies on undo logs and purge threads, while PostgreSQL depends on table versioning and VACUUM.

4. **Gap locks are central to phantom prevention.** Observing an insert inside a locked range block while another insert outside the range succeeded immediately made the purpose of gap locking much more concrete. Although this behavior can occasionally surprise developers, it is precisely what enables InnoDB's **REPEATABLE READ** isolation level to prevent phantom rows.

5. **Default isolation levels influence application behavior.** InnoDB's default **REPEATABLE READ** provides stronger consistency guarantees than PostgreSQL's default **READ COMMITTED**. Applications that behave correctly on one database may therefore exhibit different concurrency characteristics when executed on the other, even without changing any SQL.

6. **Query optimizers reflect the design philosophy of their database engine.** For the same three-table join, MySQL selected an index nested-loop strategy, whereas PostgreSQL preferred a parallel hash join. This comparison reinforces that identical SQL does not necessarily produce similar execution plans, as each optimizer makes decisions based on its own cost model, execution strategies, and architectural strengths.

---

## References

* **MySQL 8.0 Reference Manual** — Documentation covering the InnoDB Storage Engine, including clustered and secondary indexes, the buffer pool, redo and undo logging, transaction processing, locking, and consistent reads:
  [https://dev.mysql.com/doc/refman/8.0/en/innodb-storage-engine.html](https://dev.mysql.com/doc/refman/8.0/en/innodb-storage-engine.html)

* **MySQL Server Source Code** — The InnoDB implementation located under `storage/innobase/`, providing insight into the engine's internal design:
  [https://github.com/mysql/mysql-server](https://github.com/mysql/mysql-server)

* **Jeremy Cole's InnoDB Internals Series** — A detailed exploration of InnoDB pages, indexes, and storage structures:
  [https://blog.jcole.us/innodb/](https://blog.jcole.us/innodb/)

* **Mohan et al. (1992), *ARIES: A Transaction Recovery Method...*** — The foundational paper describing the recovery algorithm that inspired InnoDB's redo/undo architecture.

> *All experimental results presented in Section 6 were obtained from a live **MySQL 8.0.46 / InnoDB** environment running in Docker. While optimizer costs and execution times may vary across hardware and configurations, the architectural behaviors demonstrated remain representative of InnoDB's design.*
