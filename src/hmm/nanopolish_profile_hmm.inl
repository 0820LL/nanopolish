//---------------------------------------------------------
// Copyright 2015 Ontario Institute for Cancer Research
// Written by Jared Simpson (jared.simpson@oicr.on.ca)
//---------------------------------------------------------
//
// nanopolish_profile_hmm -- Profile Hidden Markov Model
//
inline float calculate_skip_probability(const char* sequence,
                                        const HMMInputData& data,
                                        uint32_t ki,
                                        uint32_t kj)
{
    const PoreModel& pm = data.read->pore_model[data.strand];
    const KHMMParameters& parameters = data.read->parameters[data.strand];

    uint32_t rank_i = get_rank(data, sequence, ki);
    uint32_t rank_j = get_rank(data, sequence, kj);

    GaussianParameters level_i = pm.get_scaled_parameters(rank_i);
    GaussianParameters level_j = pm.get_scaled_parameters(rank_j);

    return get_skip_probability(parameters, level_i.mean, level_j.mean);
}

inline std::vector<BlockTransitions> calculate_transitions(uint32_t num_kmers, const char* sequence, const HMMInputData& data)
{
    const KHMMParameters& parameters = data.read->parameters[data.strand];

    std::vector<BlockTransitions> transitions(num_kmers);
    
    for(uint32_t ki = 0; ki < num_kmers; ++ki) {

        // probability of skipping k_i from k_(i - 1)
        float p_skip = ki > 0 ? calculate_skip_probability(sequence, data, ki - 1, ki) : 0.0f;

        // transitions from match state in previous block
        float p_mk = p_skip;
        float p_me = (1 - p_skip) * parameters.trans_m_to_e_not_k;
        float p_mm = 1.0f - p_me - p_mk;

        // transitions from event split state in previous block
        float p_ee = parameters.trans_e_to_e;
        float p_em = 1.0f - p_ee;
        // p_ie not allowed

        // transitions from kmer skip state in previous block
        float p_kk = p_skip;
        float p_km = 1 - p_skip;
        // p_ei not allowed    

        // log-transform and store
        BlockTransitions& bt = transitions[ki];

        bt.lp_me = log(p_me);
        bt.lp_mk = log(p_mk);
        bt.lp_mm = log(p_mm);

        bt.lp_ee = log(p_ee);
        bt.lp_em = log(p_em);
        
        bt.lp_kk = log(p_kk);
        bt.lp_km = log(p_km);
    }

    return transitions;
}

// Output writer for the Forward Algorithm
class ProfileHMMForwardOutput
{
    public:
        ProfileHMMForwardOutput(FloatMatrix* p) : p_fm(p), lp_end(-INFINITY) {}
        
        //
        inline void update_4(uint32_t row, uint32_t col, float m, float e, float k, float s, float lp_emission)
        {
            float sum_1 = add_logs(m, e);
            float sum_2 = add_logs(k, s);
            float sum = add_logs(sum_1, sum_2) + lp_emission;
            set(*p_fm, row, col, sum);
        }

        // add in the probability of ending the alignment at row,col
        inline void update_end(float v, uint32_t row, uint32_t col)
        {
            lp_end = add_logs(lp_end, v);
        }

        // get the log probability stored at a particular row/column
        inline float get(uint32_t row, uint32_t col) const
        {
            return ::get(*p_fm, row, col);
        }

        // get the log probability for the end state
        inline float get_end() const
        {
            return lp_end;
        }

        inline size_t get_num_columns() const
        {
            return p_fm->n_cols;
        }

        inline size_t get_num_rows() const
        {
            return p_fm->n_rows;
        }
    
    private:
        ProfileHMMForwardOutput(); // not allowed
        FloatMatrix* p_fm;
        float lp_end;
};

// Output writer for the Viterbi Algorithm
class ProfileHMMViterbiOutput
{
    public:
        ProfileHMMViterbiOutput(FloatMatrix* pf, UInt8Matrix* pb) : p_fm(pf), p_bm(pb), lp_end(-INFINITY) {}
        
        inline void update_4(uint32_t row, uint32_t col, float m, float e, float k, float s, float lp_emission)
        {
            // probability update
            float max = std::max(std::max(m, e), 
                                 std::max(k, s));

            set(*p_fm, row, col, max + lp_emission);

            // backtrack update
            uint8_t from;
            if(max == m)
                from = PS_MATCH;
            else if(max == e)
                from = PS_EVENT_SPLIT;
            else if(max == k)
                from = PS_KMER_SKIP;
            else if(max == s)
                from = PS_PRE_SOFT;
            set(*p_bm, row, col, from);
        }
        
        // add in the probability of ending the alignment at row,col
        inline void update_end(float v, uint32_t row, uint32_t col)
        {
            if(v > lp_end) {
                lp_end = v;
                end_row = row;
                end_col = col;
            }
        }

        // get the log probability stored at a particular row/column
        inline float get(uint32_t row, uint32_t col) const
        {
            return ::get(*p_fm, row, col);
        }

        // get the log probability for the end state
        inline float get_end() const
        {
            return lp_end;
        }
        
        // get the row/col that lead to the end state
        inline void get_end_cell(uint32_t& row, uint32_t& col)
        {
            row = end_row;
            col = end_col;
        }

        inline size_t get_num_columns() const
        {
            return p_fm->n_cols;
        }

        inline size_t get_num_rows() const
        {
            return p_fm->n_rows;
        }
    
    private:
        ProfileHMMViterbiOutput(); // not allowed

        FloatMatrix* p_fm;
        UInt8Matrix* p_bm;

        float lp_end;
        uint32_t end_row;
        uint32_t end_col;
};

// Allocate a vector with the model probabilities of skipping the first i events
inline std::vector<float> make_pre_flanking(const HMMInputData& data,
                                            const KHMMParameters& parameters,
                                            const uint32_t e_start,
                                            const uint32_t num_events)
{
    std::vector<float> pre_flank(num_events + 1, 0.0f);
    
    // base cases

    // no skipping
    pre_flank[0] = log(parameters.trans_start_to_pre);

    // skipping the first event
    // this includes the transition probability into and out of the skip state
    pre_flank[1] = log(1 - parameters.trans_start_to_pre) + // transition from start to the background state
                   log_probability_background(*data.read, e_start, data.strand) + // emit from background
                   log(1 - parameters.trans_pre_self); // transition to silent pre state

    // skip the remaining events
    for(size_t i = 2; i < pre_flank.size(); ++i) {
        uint32_t event_idx = e_start + (i - 1) * data.event_stride;
        pre_flank[i] = log(parameters.trans_pre_self) +
                       log_probability_background(*data.read, event_idx, data.strand) + // emit from background
                       pre_flank[i - 1]; // this accounts for the transition from the start & to the silent pre
    
    }

    return pre_flank;
}

// Allocate a vector with the model probabilities of skipping the remaining
// events after the alignment of event i
inline std::vector<float> make_post_flanking(const HMMInputData& data,
                                             const KHMMParameters& parameters,
                                             const uint32_t e_start,
                                             const uint32_t num_events)
{
    // post_flank[i] means that the i-th event was the last one
    // aligned and the remainder should be emitted from the background model
    std::vector<float> post_flank(num_events, 0.0f);

    // base case, all events aligned
    post_flank[num_events - 1] = log(parameters.trans_start_to_pre);

    // base case, all events aligned but 1
    {
        uint32_t event_idx = e_start + (num_events - 1) * data.event_stride; // last event
        assert(event_idx == data.event_stop_idx);
        post_flank[num_events - 2] = log(1 - parameters.trans_start_to_pre) + // transition from pre to background state
                                     log_probability_background(*data.read, event_idx, data.strand) + // emit from background
                                     log(1 - parameters.trans_pre_self); // transition to silent pre state
    }

    for(int i = num_events - 3; i >= 0; --i) {
        uint32_t event_idx = e_start + (i + 1) * data.event_stride;
        post_flank[i] = log(parameters.trans_pre_self) +
                        log_probability_background(*data.read, event_idx, data.strand) + // emit from background
                        post_flank[i + 1]; // this accounts for the transition from start, and to silent pre
    }
    return post_flank;
}

// This function fills in a matrix with the result of running the HMM.
// The templated ProfileHMMOutput class allows one to run either Viterbi
// or the Forward algorithm.
template<class ProfileHMMOutput>
inline float profile_hmm_fill_generic_local(const char* sequence,
                                      const HMMInputData& data,
                                      const uint32_t e_start,
                                      ProfileHMMOutput& output)
{
    PROFILE_FUNC("profile_hmm_fill_generic")

    const KHMMParameters& parameters = data.read->parameters[data.strand];

    // Calculate number of blocks
    // A block of the HMM is a set of PS_KMER_SKIP, PS_EVENT_SPLIT, PS_MATCH
    // events for one kmer
    uint32_t num_blocks = output.get_num_columns() / PS_NUM_STATES;

    // Precompute the transition probabilites for each kmer block
    uint32_t num_kmers = num_blocks - 2; // two terminal blocks
    uint32_t last_kmer_idx = num_kmers - 1;

    std::vector<BlockTransitions> transitions = calculate_transitions(num_kmers, sequence, data);
 
    // Precompute kmer ranks
    std::vector<uint32_t> kmer_ranks(num_kmers);
    for(size_t ki = 0; ki < num_kmers; ++ki)
        kmer_ranks[ki] = get_rank(data, sequence, ki);

    size_t num_events = output.get_num_rows() - 1;

    std::vector<float> pre_flank = make_pre_flanking(data, parameters, e_start, num_events);
    std::vector<float> post_flank = make_post_flanking(data, parameters, e_start, num_events);
    
    float lp_sm, lp_ms;
    lp_sm = lp_ms = log(1.0f / num_kmers);

    // Fill in matrix
    for(uint32_t row = 1; row < output.get_num_rows(); row++) {

        // Skip the first block which is the start state, it was initialized above
        // Similarily skip the last block, which is calculated in the terminate() function
        for(uint32_t block = 1; block < num_blocks - 1; block++) {

            // retrieve transitions
            uint32_t kmer_idx = block - 1;
            BlockTransitions& bt = transitions[kmer_idx];

            uint32_t prev_block = block - 1;
            uint32_t prev_block_offset = PS_NUM_STATES * prev_block;
            uint32_t curr_block_offset = PS_NUM_STATES * block;
            
            // Emission probabilities
            uint32_t event_idx = e_start + (row - 1) * data.event_stride;
            uint32_t rank = kmer_ranks[kmer_idx];
            float lp_emission_m = log_probability_match(*data.read, rank, event_idx, data.strand);
            float lp_emission_e = log_probability_event_insert(*data.read, rank, event_idx, data.strand);
            
            // state PS_MATCH
            float m_m = bt.lp_mm + output.get(row - 1, prev_block_offset + PS_MATCH);
            float m_e = bt.lp_em + output.get(row - 1, prev_block_offset + PS_EVENT_SPLIT);
            float m_k = bt.lp_km + output.get(row - 1, prev_block_offset + PS_KMER_SKIP);

            // Only allow skips then entry to the first k-mer
            //float m_s = lp_sm + pre_flank[row - 1];
            float m_s = -INFINITY;
            output.update_4(row, curr_block_offset + PS_MATCH, m_m, m_e, m_k, m_s, lp_emission_m);

            // state PS_EVENT_SPLIT
            float e_m = bt.lp_me + output.get(row - 1, curr_block_offset + PS_MATCH);
            float e_e = bt.lp_ee + output.get(row - 1, curr_block_offset + PS_EVENT_SPLIT);
            output.update_4(row, curr_block_offset + PS_EVENT_SPLIT, e_m, e_e, -INFINITY, -INFINITY, lp_emission_e);

            // state PS_KMER_SKIP
            float k_m = bt.lp_mk + output.get(row, prev_block_offset + PS_MATCH);
            float k_k = bt.lp_kk + output.get(row, prev_block_offset + PS_KMER_SKIP);
            output.update_4(row, curr_block_offset + PS_KMER_SKIP, k_m, -INFINITY, k_k, -INFINITY, 0.0f); // no emission

            // transition from this state directly to the end of the alignment
            if(kmer_idx == last_kmer_idx) {
                float lp1 = lp_ms + output.get(row, curr_block_offset + PS_MATCH) + post_flank[row - 1];
                float lp2 = lp_ms + output.get(row, curr_block_offset + PS_EVENT_SPLIT) + post_flank[row - 1];
                float lp3 = lp_ms + output.get(row, curr_block_offset + PS_KMER_SKIP) + post_flank[row - 1];

                output.update_end(lp1, row, curr_block_offset + PS_MATCH);
                output.update_end(lp2, row, curr_block_offset + PS_EVENT_SPLIT);
                output.update_end(lp3, row, curr_block_offset + PS_KMER_SKIP);
            }

#ifdef DEBUG_LOCAL_ALIGNMENT
            printf("[%d %d] start: %.2lf  pre: %.2lf fm: %.2lf\n", event_idx, kmer_idx, m_s + lp_emission_m, pre_flank[row - 1], output.get(row, curr_block_offset + PS_MATCH));
            printf("[%d %d]   end: %.2lf post: %.2lf\n", event_idx, kmer_idx, lp_end, post_flank[row - 1]);
#endif

#ifdef DEBUG_FILL    
            printf("Row %u block %u\n", row, block);
            printf("\tTransitions: p_mx [%.3lf %.3lf %.3lf]\n", bt.lp_mm, bt.lp_me, bt.lp_mk);
            printf("\t             p_ex [%.3lf %.3lf %.3lf]\n", bt.lp_em, bt.lp_ee, 0.0f);
            printf("\t             p_lx [%.3lf %.3lf %.3lf]\n", bt.lp_km, 0.0, bt.lp_kk);

            printf("\tPS_MATCH -- Transitions: [%.3lf %.3lf %.3lf] Prev: [%.2lf %.2lf %.2lf] sum: %.2lf\n", 
                    bt.lp_mm, bt.lp_em, bt.lp_km, 
                    output.get(row - 1, prev_block_offset + PS_MATCH),
                    output.get(row - 1, prev_block_offset + PS_EVENT_SPLIT),
                    output.get(row - 1, prev_block_offset + PS_KMER_SKIP),
                    0.0f);
            printf("\tPS_EVENT_SPLIT -- Transitions: [%.3lf %.3lf] Prev: [%.2lf %.2lf] sum: %.2lf\n", 
                    bt.lp_me, bt.lp_ee,
                    output.get(row - 1, curr_block_offset + PS_MATCH),
                    output.get(row - 1, curr_block_offset + PS_EVENT_SPLIT),
                    0.0f);

            printf("\tPS_KMER_SKIP -- Transitions: [%.3lf %.3lf] Prev: [%.2lf %.2lf] sum: %.2lf\n", 
                    bt.lp_mk, bt.lp_kk,
                    output.get(row, prev_block_offset + PS_MATCH),
                    output.get(row, prev_block_offset + PS_KMER_SKIP),
                    0.0f);

            printf("\tEMISSION: %.2lf %.2lf\n", lp_emission_m, lp_emission_e);
#endif
        }
    }
    
    return output.get_end();
}

// This function fills in a matrix with the result of running the HMM.
// The templated ProfileHMMOutput class allows one to run either Viterbi
// or the Forward algorithm.
template<class ProfileHMMOutput>
inline float profile_hmm_fill_generic_global(const char* sequence,
        const HMMInputData& data,
        const uint32_t e_start,
        ProfileHMMOutput& output)
{
    PROFILE_FUNC("profile_hmm_fill_generic")

    const KHMMParameters& parameters = data.read->parameters[data.strand];

    // Calculate number of blocks
    // A block of the HMM is a set of PS_KMER_SKIP, PS_EVENT_SPLIT, PS_MATCH
    // events for one kmer
    uint32_t num_blocks = output.get_num_columns() / PS_NUM_STATES;

    // Precompute the transition probabilites for each kmer block
    uint32_t num_kmers = num_blocks - 2; // two terminal blocks

    std::vector<BlockTransitions> transitions = calculate_transitions(num_kmers, sequence, data);

    // Precompute kmer ranks
    std::vector<uint32_t> kmer_ranks(num_kmers);
    for(size_t ki = 0; ki < num_kmers; ++ki)
        kmer_ranks[ki] = get_rank(data, sequence, ki);

    // Fill in matrix
    for(uint32_t row = 1; row < output.get_num_rows(); row++) {

        // Skip the first block which is the start state, it was initialized above
        // Similarily skip the last block, which is calculated in the terminate() function
        for(uint32_t block = 1; block < num_blocks - 1; block++) {

            // retrieve transitions
            uint32_t kmer_idx = block - 1;
            BlockTransitions& bt = transitions[kmer_idx];

            uint32_t prev_block = block - 1;
            uint32_t prev_block_offset = PS_NUM_STATES * prev_block;
            uint32_t curr_block_offset = PS_NUM_STATES * block;

            // Emission probabilities
            uint32_t event_idx = e_start + (row - 1) * data.event_stride;
            uint32_t rank = kmer_ranks[kmer_idx];
            float lp_emission_m = log_probability_match(*data.read, rank, event_idx, data.strand);
            float lp_emission_e = log_probability_event_insert(*data.read, rank, event_idx, data.strand);

            // state PS_MATCH
            float m_m = bt.lp_mm + output.get(row - 1, prev_block_offset + PS_MATCH);
            float m_e = bt.lp_em + output.get(row - 1, prev_block_offset + PS_EVENT_SPLIT);
            float m_k = bt.lp_km + output.get(row - 1, prev_block_offset + PS_KMER_SKIP);
            output.update_4(row, curr_block_offset + PS_MATCH, m_m, m_e, m_k, -INFINITY, lp_emission_m);

            // state PS_EVENT_SPLIT
            float e_m = bt.lp_me + output.get(row - 1, curr_block_offset + PS_MATCH);
            float e_e = bt.lp_ee + output.get(row - 1, curr_block_offset + PS_EVENT_SPLIT);
            output.update_4(row, curr_block_offset + PS_EVENT_SPLIT, e_m, e_e, -INFINITY, -INFINITY, lp_emission_e);

            // state PS_KMER_SKIP
            float k_m = bt.lp_mk + output.get(row, prev_block_offset + PS_MATCH);
            float k_k = bt.lp_kk + output.get(row, prev_block_offset + PS_KMER_SKIP);
            output.update_4(row, curr_block_offset + PS_KMER_SKIP, k_m, -INFINITY, k_k, -INFINITY, 0.0f); // no emission

#ifdef DEBUG_FILL    
            printf("Row %u block %u\n", row, block);
            printf("\tTransitions: p_mx [%.3lf %.3lf %.3lf]\n", bt.lp_mm, bt.lp_me, bt.lp_mk);
            printf("\t             p_ex [%.3lf %.3lf %.3lf]\n", bt.lp_em, bt.lp_ee, 0.0f);
            printf("\t             p_lx [%.3lf %.3lf %.3lf]\n", bt.lp_km, 0.0, bt.lp_kk);

            printf("\tPS_MATCH -- Transitions: [%.3lf %.3lf %.3lf] Prev: [%.2lf %.2lf %.2lf] sum: %.2lf\n", 
                    bt.lp_mm, bt.lp_em, bt.lp_km, 
                    output.get(row - 1, prev_block_offset + PS_MATCH),
                    output.get(row - 1, prev_block_offset + PS_EVENT_SPLIT),
                    output.get(row - 1, prev_block_offset + PS_KMER_SKIP),
                    0.0f);
            printf("\tPS_EVENT_SPLIT -- Transitions: [%.3lf %.3lf] Prev: [%.2lf %.2lf] sum: %.2lf\n", 
                    bt.lp_me, bt.lp_ee,
                    output.get(row - 1, curr_block_offset + PS_MATCH),
                    output.get(row - 1, curr_block_offset + PS_EVENT_SPLIT),
                    0.0f);

            printf("\tPS_KMER_SKIP -- Transitions: [%.3lf %.3lf] Prev: [%.2lf %.2lf] sum: %.2lf\n", 
                    bt.lp_mk, bt.lp_kk,
                    output.get(row, prev_block_offset + PS_MATCH),
                    output.get(row, prev_block_offset + PS_KMER_SKIP),
                    0.0f);

            printf("\tEMISSION: %.2lf %.2lf\n", lp_emission_m, lp_emission_e);
#endif
        }
    }

    uint32_t last_event_row = output.get_num_rows() - 1;
    uint32_t last_aligned_block = num_blocks - 2;
    uint32_t match_state_last_block = PS_NUM_STATES * last_aligned_block + PS_MATCH;
    output.update_end(output.get(last_event_row, match_state_last_block), last_event_row, match_state_last_block);
    return output.get_end();
}

template<class ProfileHMMOutput>
inline float profile_hmm_fill_generic(const char* sequence,
        const HMMInputData& data,
        const uint32_t e_start,
        ProfileHMMOutput& output)
{
    return profile_hmm_fill_generic_local(sequence, data, e_start, output);
}
