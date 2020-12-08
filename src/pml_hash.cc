#include "pml_hash.h"

/**
 * PMLHash::PMLHash 
 * 
 * @param  {char*} file_path : the file path of data file
 * if the data file exist, open it and recover the hash
 * if the data file does not exist, create it and initial the hash
 */
PMLHash::PMLHash(const char *file_path)
{
    int is_pmem;
    size_t mapped_len;

    if ((start_addr = pmem_map_file(file_path, FILE_SIZE, PMEM_FILE_CREATE, 0666, &mapped_len, &is_pmem)) == NULL)
    {
        perror("pmem_map_file");
        exit(1);
    }
    else
    {
        overflow_addr = start_addr + FILE_SIZE / 2;
        table_arr = (pm_table *)(start_addr + sizeof(metadata));
        meta = (metadata *)start_addr;
    }
}

/**
 * PMLHash::~PMLHash 
 * 
 * unmap and close the data file
 */
PMLHash::~PMLHash()
{
    pmem_persist(start_addr, FILE_SIZE);
    pmem_unmap(start_addr, FILE_SIZE);
}

/**
 * PMLHash 
 * 
 * @param  {pm_table *} addr : address of hash table to be inserted
 * @param  {entry} en        : the entry to insert
 * @return {int}             : success: 0. fail: -1
 * TODO
 */
int PMLHash::insert_bucket(pm_table *addr, entry en)
{
    pm_table *table = addr;
    while (table->next_offset != 0)
    {
        table = (pm_table *)table->next_offset;
    }
    if (table->fill_num >= 16)
    {
        uint64_t offset = (FILE_SIZE / 2) + (meta->overflow_num * sizeof(pm_table));
        table->next_offset = (uint64_t)newOverflowTable(offset);
        if (table->next_offset == 0)
        {
            return -1;
        }
        
        table = (pm_table *)table->next_offset;
    }
    table->kv_arr[table->fill_num] = en;
    table->fill_num++;
    table->next_offset = 0;
    return 0;
}

/**
 * PMLHash 
 * 
 * split the hash table indexed by the meta->next
 * update the metadata
 */
void PMLHash::split()
{
    // fill the split table
    vector<entry> temp_arr;
    int hash_num = (2 << (meta->level)) * HASH_SIZE * 2;
    pm_table *split_table = &table_arr[meta->next];
    while (true)
    {
        for (uint64_t i = 0; i < split_table->fill_num; i++)
        {
            uint64_t tag = hashFunc(split_table->kv_arr[i].key, hash_num);
            // move to the new bucket
            if (tag != meta->next)
            {
                pm_table *new_table = &table_arr[tag];
                entry en = {
                    key : split_table->kv_arr[i].key,
                    value : split_table->kv_arr[i].value
                };
                insert_bucket(new_table, en);
            }
            // stay in the old bucket, move to temp_arr first
            else
            {
                entry en = {
                    key : split_table->kv_arr[i].key,
                    value : split_table->kv_arr[i].value
                };
                temp_arr.push_back(en);
            }
        }
        if (split_table->next_offset == 0)
            break;
        split_table = (pm_table *)(split_table->next_offset);
    }

    // fill the new table
    split_table = &table_arr[meta->next];
    split_table->fill_num = 0;
    for (size_t i = 0; i < temp_arr.size(); i++)
    {
        if (split_table->fill_num >= 16)
        {
            split_table = (pm_table *)split_table->next_offset;
            split_table->fill_num = 0;
        }
        split_table->kv_arr[split_table->fill_num] = temp_arr[i];
    }
    split_table->next_offset = 0;

    // update the next of metadata
    meta->next++;
    if (meta->next > ((2 << meta->level) * HASH_SIZE))
    {
        meta->next = 0;
        meta->level++;
    }

    pmem_persist(start_addr, FILE_SIZE);
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key     : key
 * @param  {size_t} hash_size : the N in hash func: idx = hash % N
 * @return {uint64_t}         : index of hash table array
 * 
 * need to hash the key with proper hash function first
 * then calculate the index by N module
 */
uint64_t PMLHash::hashFunc(const uint64_t &key, const size_t &hash_size)
{
    return (key * 3) % hash_size;
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} offset : the file address offset of the overflow hash table
 *                             to the start of the whole file
 * @return {pm_table*}       : the virtual address of new overflow hash table
 */
pm_table *PMLHash::newOverflowTable(uint64_t &offset)
{
    if(offset > FILE_SIZE)
        return NULL;
    pm_table *new_overflow_table = (pm_table *)(start_addr + offset);
    return new_overflow_table;
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key   : inserted key
 * @param  {uint64_t} value : inserted value
 * @return {int}            : success: 0. fail: -1
 * 
 * insert the new kv pair in the hash
 * 
 * always insert the entry in the first empty slot
 * 
 * if the hash table is full then split is triggered
 */
int PMLHash::insert(const uint64_t &key, const uint64_t &value)
{
    return 0; //for test
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key   : the searched key
 * @param  {uint64_t} value : return value if found
 * @return {int}            : 0 found, -1 not found
 * 
 * search the target entry and return the value
 */
int PMLHash::search(const uint64_t &key, uint64_t &value)
{
    size_t i = 2 << (meta->level);
    uint64_t t = hashFunc(key, i * 16);
    int len = table_arr[t].fill_num;

    //search the t-th hash table
    for (int o = 0; o < len; o++)
    {
        if (table_arr[t].kv_arr[o].key == key)
        {
            value = table_arr[t].kv_arr[o].value;
            return 0;
        }
    }
    //search the t-th overflow table
    //if the hash table dose not exist the value and the overflow table exists
    if (table_arr[t].fill_num == 16 && table_arr[t].next_offset != 0)
    {
        pm_table *temp = (pm_table *)table_arr[t].next_offset;
        int overflow_len = temp->fill_num;
        for (int o = 0; o < overflow_len; o++)
        {
            if (temp->kv_arr[o].key == key)
            {
                value = temp->kv_arr[o].value;
                return 0;
            }
        }
    }
    return -1;
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key : target key
 * @return {int}          : success: 0. fail: -1
 * 
 * remove the target entry, move entries after forward
 * if the overflow table is empty, remove it from hash
 */
int PMLHash::remove(const uint64_t &key)
{
    return 0; //for test
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key   : target key
 * @param  {uint64_t} value : new value
 * @return {int}            : success: 0. fail: -1
 * 
 * update an existing entry
 */
int PMLHash::update(const uint64_t &key, const uint64_t &value)
{
    size_t i = 2 << (meta->level);
    uint64_t t = hashFunc(key, i * 16);
    int len = table_arr[t].fill_num;

    // search the t-th hash table
    for (int o = 0; o < len; o++)
    {
        if (table_arr[t].kv_arr[o].key == key)
        {
            table_arr[t].kv_arr[o].value = value;
            pmem_persist(start_addr, FILE_SIZE);
            return 0;
        }
    }
    // search the t-th overflow table
    // if the hash table dose not exist the value and the overflow table exists
    if (table_arr[t].fill_num == 16 && table_arr[t].next_offset != 0)
    {
        pm_table *temp = (pm_table *)table_arr[t].next_offset;
        int overflow_len = temp->fill_num;
        for (int o = 0; o < overflow_len; o++)
        {
            if (temp->kv_arr[o].key == key)
            {
                temp->kv_arr[o].value = value;
                pmem_persist(start_addr, FILE_SIZE);
                return 0;
            }
        }
    }
    return -1;
}