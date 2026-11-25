#define _POSIX_C_SOURCE 200809L
#include "distribution_policy.h"
#include <errno.h>
#include <string.h>
static int invalid(int error) {errno=error;return -1;}
int parse_distribution_policy(const char *text,enum distribution_policy *policy) {
    if(!text || !policy)return invalid(EINVAL);
    static const char *const names[]={"rr","least-outstanding","response-time","combined"};
    for(size_t i=0;i<4;++i)if(!strcmp(text,names[i])){*policy=(enum distribution_policy)i;return 0;}
    return invalid(EINVAL);
}
bool valid_distribution_observation(const struct distribution_observation *sample) {
    return sample && sample->result>=DISTRIBUTION_NO_SAMPLE && sample->result<=DISTRIBUTION_CANCELLED &&
        (sample->result==DISTRIBUTION_NO_SAMPLE ? (!sample->elapsed_us && !sample->finished_us) :
         sample->elapsed_us<=sample->finished_us);
}
int distribution_elapsed_us(const struct timespec *start,const struct timespec *finish,
    struct distribution_observation *sample,enum distribution_sample_result result) {
    if(!start || !finish || !sample || result<DISTRIBUTION_COMPLETED || result>DISTRIBUTION_CANCELLED ||
       start->tv_sec<0 || finish->tv_sec<0 || start->tv_nsec<0 || finish->tv_nsec<0 ||
       start->tv_nsec>=1000000000L || finish->tv_nsec>=1000000000L)return invalid(EINVAL);
    if(finish->tv_sec<start->tv_sec || (finish->tv_sec==start->tv_sec && finish->tv_nsec<start->tv_nsec))return invalid(ERANGE);
    if((uint64_t)finish->tv_sec>(UINT64_MAX-999999)/1000000)return invalid(EOVERFLOW);
    uint64_t seconds=(uint64_t)(finish->tv_sec-start->tv_sec);
    long nanoseconds=finish->tv_nsec-start->tv_nsec;
    if(nanoseconds<0){--seconds;nanoseconds+=1000000000L;}
    uint64_t elapsed=seconds*1000000+(uint64_t)nanoseconds/1000;
    uint64_t end=(uint64_t)finish->tv_sec*1000000+(uint64_t)finish->tv_nsec/1000;
    *sample=(struct distribution_observation){elapsed,end,result};return 0;
}
int update_distribution_timing(struct distribution_timing *timing,const struct distribution_observation *sample) {
    if(!timing || !valid_distribution_observation(sample))return invalid(EINVAL);
    if(sample->result==DISTRIBUTION_NO_SAMPLE || sample->result==DISTRIBUTION_CANCELLED)return 0;
    uint64_t value=sample->elapsed_us?sample->elapsed_us:1;
    if(sample->result==DISTRIBUTION_FAILED && value<DISTRIBUTION_FAILURE_FLOOR_US)value=DISTRIBUTION_FAILURE_FLOOR_US;
    if(!timing->representative_us)timing->representative_us=value;
    else if(value>=timing->representative_us)timing->representative_us+=(value-timing->representative_us)/DISTRIBUTION_EWMA_DIVISOR;
    else timing->representative_us-=(timing->representative_us-value)/DISTRIBUTION_EWMA_DIVISOR;
    if(sample->finished_us>timing->last_finished_us)timing->last_finished_us=sample->finished_us;
    return 0;
}
static uint64_t combined_score(uint64_t outstanding,uint64_t time) {
    if(outstanding==UINT64_MAX || time>UINT64_MAX/(outstanding+1))return UINT64_MAX;
    return (outstanding+1)*time;
}
int choose_distribution_candidate(enum distribution_policy policy,
    const struct distribution_candidate *candidates,size_t count,
    const struct distribution_turn *turn,size_t *selected,struct distribution_turn *next) {
    if(!candidates || !turn || !selected || !next || !count || count>UINT32_MAX ||
       turn->cursor>=count || turn->selections>=DISTRIBUTION_EXPLORE_INTERVAL ||
       policy<DISTRIBUTE_RR || policy>DISTRIBUTE_COMBINED)return invalid(EINVAL);
    bool timed=policy==DISTRIBUTE_RESPONSE_TIME || policy==DISTRIBUTE_COMBINED;
    bool explore=timed && turn->selections==DISTRIBUTION_EXPLORE_INTERVAL-1;
    bool unknown=false;
    if(timed && !explore)for(size_t i=0;i<count;++i)
        if(candidates[i].eligible && !candidates[i].timing.representative_us)unknown=true;
    size_t found=count;uint64_t best=UINT64_MAX;
    for(size_t offset=0;offset<count;++offset){
        size_t i=((size_t)turn->cursor+offset)%count;
        const struct distribution_candidate *candidate=&candidates[i];
        if(!candidate->eligible || (unknown && candidate->timing.representative_us))continue;
        uint64_t score=0;
        if(!explore && !unknown){
            if(policy==DISTRIBUTE_LEAST_OUTSTANDING)score=candidate->outstanding;
            else if(policy==DISTRIBUTE_RESPONSE_TIME)score=candidate->timing.representative_us;
            else if(policy==DISTRIBUTE_COMBINED)score=combined_score(candidate->outstanding,candidate->timing.representative_us);
        }
        if(found==count || score<best){found=i;best=score;}
    }
    if(found==count)return invalid(EAGAIN);
    *selected=found;*next=(struct distribution_turn){(uint32_t)((found+1)%count),
        timed?(turn->selections+1)%DISTRIBUTION_EXPLORE_INTERVAL:0};return 0;
}
