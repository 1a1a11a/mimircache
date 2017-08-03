//
//  cacheServer.cpp
//  akamaiSimulator
//
//  Created by Juncheng on 7/11/17.
//  Copyright © 2017 Juncheng. All rights reserved.
//


#include "cacheServer.hpp"
#include <iostream>

namespace akamaiSimulator {
    
    
    /*************************** cacheServerStat ****************************
     this class records the statistics of the cache server 
     ************************************************************************/
    
    cacheServerStat::cacheServerStat(const unsigned long server_id,
                                     const unsigned long cache_size,
                                     unsigned long* layer_size) {
        
        
        unsigned long i;
        
        this->cache_server_id = server_id;
        this->cache_size = cache_size;
        this->num_req = 0;
        this->num_hit = 0;
        this->layer_size = layer_size;
        
        for (i=0; i<NUM_CACHE_LAYERS; i++){
//            this->layer_size[i] = layer_size[i];
            this->num_req_per_layer[i] = 0;
            this->num_hit_per_layer[i] = 0;
        }
    }
    
    
    // copy constructor
    cacheServerStat::cacheServerStat(const cacheServerStat& stat){
        *this = stat;
    }
    
    
    
    cacheServerStat::~cacheServerStat(){
        ;
    }
    
    
    cacheServerStat& cacheServerStat::operator= (const cacheServerStat& stat){
        if (this == &stat)
            return *this;
        this->cache_server_id = stat.cache_server_id;
        this->cache_size = stat.cache_size;
        this->num_req = stat.num_req;
        this->num_hit = stat.num_hit;
        this->layer_size = stat.layer_size;
        
        for (unsigned int i=0; i<NUM_CACHE_LAYERS; i++){
//            this->layer_size[i] = stat.layer_size[i];
            this->num_req_per_layer[i] = stat.num_req_per_layer[i];
            this->num_hit_per_layer[i] = stat.num_hit_per_layer[i];
        }
        
        return *this;
    }
    
//    void cacheServerStat::set_new_boundaries(double *boundaries){
//        for (unsigned int i=0; i<NUM_CACHE_LAYERS; i++){
//            this->layer_size[i] = (unsigned long) (boundaries[i] * cache_size);
//        }
//    }

    
    
    /************************** cacheProfiler ***************************
     this class handles the dynamic boundary inside cache server 
     ********************************************************************/
    
    cacheProfiler::cacheProfiler(const unsigned long max_cache_size,
                                 const unsigned char data_type,
                                 const unsigned long *layer_size,
                                 const unsigned long adjust_interval,
                                 const unsigned long L1_latency,
                                 const unsigned long L2_latency,
                                 const unsigned long Lo_latency){
        this->__init(max_cache_size, data_type,
                     layer_size, adjust_interval,
                     L1_latency, L2_latency, Lo_latency);
    }
    
    
    void cacheProfiler::__init(const unsigned long max_cache_size,
                               const unsigned char data_type,
                               const unsigned long *layer_size,
                               const unsigned long adjust_interval,
                               const unsigned long L1_latency,
                               const unsigned long L2_latency,
                               const unsigned long Lo_latency){
        unsigned long i;
        this->last_adjust_ts = 0;
        this->L1_latency = L1_latency;
        this->L2_latency = L2_latency;
        this->Lo_latency = Lo_latency;
        
        this->max_cache_size = max_cache_size;
        this->data_type = data_type;
        this->adjust_interval = adjust_interval;
        
        
        for (i=0; i<NUM_CACHE_LAYERS; i++){
            this->layer_size[i] = layer_size[i];
            this->rd_count_array_size[i] = max_cache_size+3;
            this->ts[i] = 0;
            this->rd_count_array[i] = new unsigned long long[this->rd_count_array_size[i]];
            memset(this->rd_count_array, 0, sizeof(unsigned long long) *
                        (this->rd_count_array_size[i]));
            
            if (data_type == 'c')
                this->hashtable[i] = g_hash_table_new_full(g_str_hash, g_str_equal,
                                    (GDestroyNotify)simple_g_key_value_destroyer,
                                    (GDestroyNotify)simple_g_key_value_destroyer);

            else if (data_type == 'l')
                this->hashtable[i] = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                    (GDestroyNotify)simple_g_key_value_destroyer,
                                    (GDestroyNotify)simple_g_key_value_destroyer);
                
            else{
                error_msg("unknown data type %c\n", data_type);
                abort();
            }
            
            this->splay_tree[i] = NULL;
        }
    }
    
    
    
    
    
    int cacheProfiler::add_request(const cache_line_t *const cp,
                                   const unsigned long layer_id){
        
        long long reuse_dist;
        
        /* find out the reuse distance of current request, and add to couter */
        this->splay_tree[layer_id-1] =
            this->process_one_element(cp, this->splay_tree[layer_id-1],
                                      this->hashtable[layer_id-1],
                                      this->ts[layer_id-1], &reuse_dist);

        
        if (reuse_dist == -1)
            (this->rd_count_array[layer_id-1][this->rd_count_array_size[layer_id-1]-1]) ++;
        else if (reuse_dist > (long long) (this->max_cache_size))
            this->rd_count_array[layer_id-1][this->rd_count_array_size[layer_id-1]-2] += 1;
        else
            this->rd_count_array[layer_id-1][reuse_dist] += 1;

        this->ts[layer_id-1] ++;

    
        /** now check whether need to recalculate boundary, 
         *  if yes, how to shift the boundary */
        bool need_to_adjust = true;
        for (unsigned long i=0; i<NUM_CACHE_LAYERS; i++){
            if (this->ts[i] - this->last_adjust_ts < this->adjust_interval){
                need_to_adjust = false;
                break;
            }
        }
        
        
        if (need_to_adjust) {
            /* the logic for checking how to adjust boundary */
            
            return this->__how_to_adjust();
            
        }
        else
            return 0;
    }


    
    int cacheProfiler::__how_to_adjust(){
        /** delta1 is the latency benefit of increasing L1,
         *  less than 0, then shift
         *  (Lo−L2) * RD_2(C_2) − (Lo−L1) * RD_1(C_1+1)
         *  delta2 is the latency benefit of increasing L2. 
         *  (Lo−L1) * RD_1(C_1) - (Lo−L2) * RD_2(C_2+1)
         */
        unsigned long delta1, delta2;
        delta1 = (this->Lo_latency - this->L2_latency) *
        this->rd_count_array[1][this->layer_size[1]] -
        (this->Lo_latency - this->L1_latency) *
        this->rd_count_array[0][this->layer_size[0] + 1];
        
        delta2 = (this->Lo_latency - this->L1_latency) *
        this->rd_count_array[0][this->layer_size[0]] -
        (this->Lo_latency - this->L2_latency) *
        this->rd_count_array[1][this->layer_size[1] + 1];
        
        if (delta1 < 0 && delta2 < 0){
            throw std::runtime_error("delta1 and delta2 both smaller than 0");
        }

        if (delta1 > 0 && delta2 > 0){
            throw std::runtime_error("delta1 and delta2 both greater than 0");
        }

        if (delta1 < 0)
            return 1;
        
        if (delta2 > 0)
            return -1;
        
        return -2;
    }

    
    void cacheProfiler::set_new_boundaries(unsigned long *layer_size){
        for (unsigned long i=0; i<NUM_CACHE_LAYERS; i++)
            this->layer_size[i] = layer_size[i];
    }

    void cacheProfiler::clear(){
        for (unsigned long i=0; i<NUM_CACHE_LAYERS; i++){
            g_hash_table_destroy(this->hashtable[i]);
            free_sTree(this->splay_tree[i]);
        }
        
        this->__init(this->max_cache_size, this->data_type,
                     this->layer_size, this->adjust_interval,
                     this->L1_latency, this->L2_latency,
                     this->Lo_latency);
    }
    
    
    cacheProfiler::~cacheProfiler(){
        for (unsigned long i=0; i<NUM_CACHE_LAYERS; i++){
            g_hash_table_destroy(this->hashtable[i]);
            free_sTree(this->splay_tree[i]);
            delete [] this->rd_count_array[i];
        }
    }
    
    
    
    
    /************************** cacheServer *****************************
     this class is the cache server class, it simulates the several cache layers
     inside the one cache server
     ********************************************************************/
    
    cacheServer::cacheServer(const unsigned long server_id,
                             akamaiStat* const akamai_stat,
                             bool dynamic_boundary_flag,
                             const unsigned long size,
                             const double* const boundaries,
                             const cache_type cache_alg,
                             const unsigned char data_type,
                             const int block_size,
                             void *params,
                             const std::string server_name){
        
        
        unsigned long i;

        this->dynamic_boundary_flag = dynamic_boundary_flag;
        this->cache_server_id = server_id;
        this->cache_server_name = server_name;
        this->cache_size = size;
        this->akamai_stat = akamai_stat;
        for (i=0; i<NUM_CACHE_LAYERS; i++)
            this->layer_size[i] = (unsigned long) (this->cache_size * boundaries[i]);
        
        this->server_stat = new cacheServerStat(server_id, size, this->layer_size);
        
        
        
        switch (cache_alg) {
            case e_LRU:
                for (i=0; i<NUM_CACHE_LAYERS; i++)
                    this->caches[i] = LRU_init(this->layer_size[i],
                                               data_type, block_size, params);
                
                break;
                
            default:
                error_msg("given algorithm is not supported: %d\n", cache_alg);
                break;
        }
        
        
        
        if (dynamic_boundary_flag)
            this->cache_profiler = new cacheProfiler(this->cache_size,
                                                     data_type,
                                                     this->layer_size);
        else
            this->cache_profiler = NULL;

        
    }
    
    
    /** use an array of caches to initialize the cache server,
     *  the first cache in the array is layer 1 cache, second is layer 2, etc.
     *  ATTENTION: the life of caches is handed over to cache server,
     *  thus resource de-allocation will be done by cacheServer */
    cacheServer::cacheServer(const unsigned long id,
                             const cache_t ** const caches,
                             bool dynamic_boundary_flag,
                             akamaiStat* const akamai_stat,
                             const std::string server_name){

        this->dynamic_boundary_flag = dynamic_boundary_flag;
        this->cache_server_id = id;
        this->cache_server_name = server_name;
        this->set_caches(caches);
        this->akamai_stat = akamai_stat;
        
        if (dynamic_boundary_flag)
            this->cache_profiler = new cacheProfiler(this->cache_size,
                                                     this->caches[0]->core->data_type,
                                                     this->layer_size);
        else
            this->cache_profiler = NULL;
        
        
        this->server_stat = new cacheServerStat(id, this->cache_size,
                                                this->layer_size);
    }
    
    
    
    
    /******************* setter *******************/
    bool cacheServer::set_L1cache(const cache_t * const cache){
        this->caches[0] = (cache_t*) cache;
        if ((unsigned long) cache->core->size != this->server_stat->layer_size[0]){
            error_msg("new L1 cache size is different, %lu %ld\n",
                      this->server_stat->layer_size[0], cache->core->size);
            abort();
        }
        return TRUE;
    }
    
    
    bool cacheServer::set_L2cache(const cache_t * const cache){
        this->caches[1] = (cache_t*) cache;
        if ((unsigned long) cache->core->size != this->server_stat->layer_size[1]){
            error_msg("new L2 cache size is different, %lu %ld\n",
                      this->server_stat->layer_size[0], cache->core->size);
            abort();
        }

        return TRUE;
    }
    
    bool cacheServer::set_Lncache(int n, const cache_t* const cache){
        if (n > NUM_CACHE_LAYERS){
            error_msg("specified %d th cache, but only %d layer of cache\n", n, NUM_CACHE_LAYERS);
            abort();
        }
        this->caches[n-1] = (cache_t*) cache;
        if ((unsigned long) cache->core->size != this->server_stat->layer_size[n-1]){
            error_msg("new Ln cache size is different, %lu %ld\n",
                      this->server_stat->layer_size[0], cache->core->size);
            abort();
        }

        return TRUE;
    }

    
    bool cacheServer::set_caches(const cache_t ** const caches){
        this->cache_size = 0;
        for (int i=0; i<NUM_CACHE_LAYERS; i++){
            this->caches[i] = (cache_t*) (caches[i]);
            this->layer_size[i] = (unsigned long) (this->caches[i]->core->size);
            this->cache_size += this->layer_size[i];
        }

        return TRUE;
    }
    
    
//    bool cacheServer::set_boundary(const double* const boundaries){
//        for (int i=0; i<NUM_CACHE_LAYERS; i++)
//            this->boundaries[i] = boundaries[i];
//        this->adjust_caches();
//        
//        info("reset boundary of cache server %s (id %lu), new size %ld, size of each layer ",
//             this->cache_server_name.c_str(), this->cache_server_id, this->cache_size);
//        for (int i=0; i<NUM_CACHE_LAYERS; i++)
//            printf("%ld, ", this->caches[i]->core->size);
//        printf("\n");
//        
//        return TRUE;
//    }
    
    bool cacheServer::set_size(const unsigned long size){
        /* if shrinking size of cache,
         * the actual shrinking is done when next request comes in */
        
        this->cache_size = size;
        this->adjust_caches();
        
        info("reset size of cache server %s (id %lu), new size %ld, size of each layer ",
             this->cache_server_name.c_str(), this->cache_server_id, this->cache_size);
        for (int i=0; i<NUM_CACHE_LAYERS; i++)
            printf("%ld, ", this->caches[i]->core->size);
        printf("\n");
        
        return TRUE;
    }
    
    
    
    /** this function is used internally for adjust the real cache size of each layer **/ 
    bool cacheServer::adjust_caches(){
        int i;
        for (i=0; i<NUM_CACHE_LAYERS; i++){
            this->caches[i]->core->size = (long) (this->layer_size[i]);
        }
        return TRUE;
    }
    
    
    
    
    /** add a request to certain layer 
     *  \param cp          cache request struct
     *  \param layer_id    the index of cache layer which begins from 1
     *  \return            whether it is a hit or miss on this layer 
     */
    bool cacheServer::add_request(const cache_line_t *const cp,
                                      const unsigned long layer_id){
        /* log server stat */
        this->server_stat->num_req_per_layer[layer_id-1] ++;
        this->server_stat->num_req ++;
        
        /* also write stat into akamaiStat */
        this->akamai_stat->req[layer_id-1]++;

        bool hit = this->caches[0]->core->add_element(this->caches[layer_id-1],
                                                   (cache_line_t*) cp);
        
        if (hit){
            this->server_stat->num_hit ++;
            this->server_stat->num_hit_per_layer[layer_id-1] ++;
            this->akamai_stat->hit[layer_id-1]++; 
        }
        return hit; 
    }
    
    
//    void cacheServer::dynamic_adjust(){
//        
//        
//        
//    }
    
    
    
    /******************* getter *******************/
    
    
    unsigned long cacheServer::get_cache_size(){
        return this->cache_size;
    }
    
    unsigned long* cacheServer::get_layer_size(){
        return this->server_stat->layer_size;
    }
    
    
    unsigned long cacheServer::get_server_id(){
        return this->cache_server_id;
    }
        
    
    bool cacheServer::verify(){
        error_msg("verify is not implemented"); 
        return FALSE;
    }
    
    
    
    cacheServer::~cacheServer(){
        for (int i=0; i<NUM_CACHE_LAYERS; i++)
            this->caches[i]->core->destroy(this->caches[i]);
        delete this->server_stat;
    }
    
    
}
