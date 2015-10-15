#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>

#include <time.h>
#include <limits.h>
#include <getopt.h>
#include <errno.h>

#define BUFFER_SIZE 80
#define MAX_PROBES 16
#define AVERAGER_SIZE 400

//window size for averaging
int average_n = 10;//0.025s * 400 Hz

int verbose = 0;

#define DETAIL 10

//average calculation struct
//uses circular buffer and calculates average, median and root-square-mean deviation
typedef struct average_data {
        int data_count;
        int circular_index;
        int buffer[AVERAGER_SIZE];

        double average;
        double rmsd;

        int median;
        int min_value;
        int max_value;
        int min_value_filtered;
        int max_value_filtered;        
} average_data_t;


//init averaging structure
void init_average(average_data_t *avg){
        memset(avg, 0, sizeof(average_data_t));
        avg->min_value = INT_MAX;
        avg->max_value = INT_MIN;
        avg->min_value_filtered = INT_MAX;
        avg->max_value_filtered = INT_MIN;
}


int min(int a, int b) {
    if (a > b)
        return b;
    return a;
}

int max(int a, int b) {
    if (a < b)
        return b;
    return a;
}


//qsort comparer for median filter
int cmp_int(const void *p1, const void *p2){
        return (*(int*)p1)-(*(int*)p2);
}


//calculate averages for array of 'size' ints
void update_average_from_array(average_data_t *avg, int* array, int size){
        double average = 0;
        for(int i=0;i<size;i++){
                average+=array[i];
        }
        avg->average = average/size;

        double rmsd = 0;
        for(int i=0;i<size;i++){
                double tmp = array[i]-avg->average;
                rmsd+=tmp*tmp;
        }
        avg->rmsd = sqrt(rmsd/size);
}


//average has enough samples to work with?
bool average_has_enough_data(average_data_t *avg){
        return avg->data_count>=average_n;
}


//update averages with new sample
void update_average(average_data_t *avg, int value){

        avg->buffer[avg->circular_index]=value;
        avg->data_count++;
        avg->circular_index++;
        avg->min_value = min(avg->min_value, value);
        avg->max_value = max(avg->max_value, value);

        if(avg->data_count>=average_n){
                int median[AVERAGER_SIZE];
                int median_idx = 0;
                if(avg->circular_index>=average_n){
                        for(int i=avg->circular_index-average_n;i<avg->circular_index;i++){
                                median[median_idx++]=avg->buffer[i];
                        }
                }else{
                        for(int i=AVERAGER_SIZE-(average_n-avg->circular_index);i<AVERAGER_SIZE;i++){
                                median[median_idx++]=avg->buffer[i];
                        }
                        for(int i=0;i<avg->circular_index;i++){
                                median[median_idx++]=avg->buffer[i];
                        }
               }
               assert(median_idx==average_n);
               update_average_from_array(avg, median, average_n);

               qsort(median, average_n, sizeof(int), cmp_int);
               avg->median = median[average_n/2];
               avg->min_value_filtered = min(avg->min_value_filtered, avg->median);
               avg->max_value_filtered = max(avg->max_value_filtered, avg->median);

        }

        if(avg->circular_index>=AVERAGER_SIZE){
                avg->circular_index = 0;
        }
}

//probe(logic input) data and timing
typedef struct probe_data{
        int time;
        int last_value;

        int rising_edge_time;
        int falling_edge_time;
        int pulse_count;

        average_data_t pulse_width_avg;
        average_data_t period_avg;

} probe_data_t;

bool probe_has_enough_data(probe_data_t *probe){
        return probe->pulse_count>=average_n;
}

//working context, all inputs/probes data
typedef struct context{
        int probes_n;
        probe_data_t probes[MAX_PROBES];
        int max_time;
} context_t;



//convert char to hex digit
int from_hex(char digit){
        if(digit>='0' && digit<='9'){
                return digit-'0';
        }
        if(digit>='a' && digit<='f'){
                return digit-'a'+10;
        }
        if(digit>='A' && digit<='F'){
                return digit-'A'+10;
        }
        return -1;
}

//feed 4 bit from probe to edge detector and timing calculation
int feed_data(probe_data_t *data, int digit){
        if(digit<0 || digit>15){
                return 3;
        }
        if(verbose>DETAIL) {
                fprintf(stderr, "%4x", digit);
        }

        for(int i=0;i<3;i++){
                int value = digit>>3;
                if(data->last_value!=0 && value==0){
                        //falling edge
                        if(data->rising_edge_time>=0){
                                update_average(&data->pulse_width_avg, data->time - data->rising_edge_time);
                        }
                        data->falling_edge_time = data->time;
                }
                if(data->last_value==0 && value!=0){
                        //rising edge
                        if(data->rising_edge_time>=0){
                                update_average(&data->period_avg, data->time - data->rising_edge_time);
                                data->pulse_count++;
                        }
                        data->rising_edge_time = data->time;
                }

                data->last_value = value;
                data->time++;
                digit<<=1;
        }
        return 0;
}

//process probe data line
int process_probe(context_t *ctx, int probe_idx, char buffer[]){
        if(probe_idx<0 || probe_idx>=MAX_PROBES){
                return 1;
        }
        if(probe_idx>=ctx->probes_n){
                int i = ctx->probes_n;
                while(i<=probe_idx){
                        //TODO: different start time for probes?
                        probe_data_t *probe = &ctx->probes[i];
                        probe->time = 0;
                        probe->last_value = 0;
                        probe->rising_edge_time = -1;
                        probe->falling_edge_time = -1;
                        probe->pulse_count = 0;
                        init_average(&probe->period_avg);
                        init_average(&probe->pulse_width_avg);
                        i++;
                }
                ctx->probes_n = probe_idx+1;
        }
        if(verbose>DETAIL) {
                fprintf(stderr, "probe: %d ", probe_idx);
        }

        int p=2;
        probe_data_t *probe = &ctx->probes[probe_idx];
        int time = probe->time;
        while(p<BUFFER_SIZE-3 && buffer[p]!='\n' && buffer[p]!=0){
                int digit_h = from_hex(buffer[p]);
                int digit_l = from_hex(buffer[p+1]);
                char space = buffer[p+2];
                if(space!=' ' || digit_h<0 || digit_l<0){
                        return 2;
                }

                int r;
                r = feed_data(probe, digit_h);
                if(r){
                        fprintf(stderr, "feed_data digit_h error %d", r);
                        return r;
                }
                feed_data(probe, digit_l);
                if(r){
                        fprintf(stderr, "feed_data digit_l error %d", r);
                        return r;
                }
                time = probe->time;
                p+=3;
        }
        if(time>ctx->max_time){
                ctx->max_time = time;//where is my max?
        }
        return 0;
}

//calc timespec diff in ms
int diffts(struct timespec start, struct timespec end)
{
    return (end.tv_sec-start.tv_sec)*1000 +
           (end.tv_nsec-start.tv_nsec)/1000000;
}


//dump averaging result
void dump_average(char *name, average_data_t *avg, int samplerate){
        printf("%s:", name);
        if(!average_has_enough_data(avg)){
                printf("no data\n");
                return;
        }
        printf("avg:%f rmsd:%f median:%d min:%d max:%d min_f:%d max_f:%d\n", avg->average, avg->rmsd, avg->median, 
                        avg->min_value, avg->max_value, avg->min_value_filtered, avg->max_value_filtered);

        if(samplerate>0) {
                printf("%s:", name);
                double koeff = 1000.0/samplerate;
                printf("avg:%f rmsd:%f median:%f min:%f max:%f min_f:%f max_f:%f\n", avg->average*koeff, avg->rmsd*koeff, avg->median*koeff, 
                                avg->min_value*koeff, avg->max_value*koeff, avg->min_value_filtered*koeff, avg->max_value_filtered*koeff);
        }
        printf("\n");
}

//dump probe averages
void dump_result(context_t *ctx, int samplerate){
        for(int i=0;i<ctx->probes_n;i++){
                probe_data_t *probe = &ctx->probes[i];
                printf("probe: %d", i);
                if(!probe_has_enough_data(probe)){
                        printf("no data\n");
                        continue;
                }
                printf(" pulses:%d\n", probe->pulse_count);
                dump_average("  width:  ", &probe->pulse_width_avg, samplerate);
                dump_average("  period: ", &probe->period_avg, samplerate);
        }
}


//process incoming data
int process_data(context_t *ctx, int samplerate){

        char buffer[BUFFER_SIZE];
        bool started=false;
        int line_num=1;

        struct timespec lts;
        if(clock_gettime(CLOCK_MONOTONIC, &lts)!=0){
                fprintf(stderr, "error get clock %s\n", strerror(errno));
                return 1;
        }
        while(fgets(buffer, BUFFER_SIZE, stdin)){
                //printf("%p %s\n", buffer, buffer);
                char probe = buffer[0];
                bool is_data = (probe>='0' && probe<='9' && buffer[1]==':');

                started |= is_data;
                if(started && !is_data){
                        fprintf(stderr, "error, no data line %d %s",line_num, buffer);
                        return 1;
                }
                if(started){
                        int r = process_probe(ctx, probe-'0', buffer);
                        if(r){
                                fprintf(stderr, "process_probe error %d line:%d %s", r, line_num, buffer);
                                return r;
                        }
                        struct timespec ts;
                        if(clock_gettime(CLOCK_MONOTONIC, &ts)!=0){
                                fprintf(stderr, "error get clock %s\n", strerror(errno));
                                return 1;
                        }
                        if(diffts(lts, ts)>250){
                                printf("\x1B[1;1H");
                                printf("%d\n", line_num);
                                dump_result(ctx, samplerate);
                                lts=ts;
                        }

                }
                line_num++;
        }
        return 0;
}


//usage help
void show_help(){
        printf("Usage: pwm [-n buffer_length] [-s sample_rate_khz] [-v] [-h] < sigrok_hex_file\n");
}


//main entry point
int main(int argc, char** argv){
        context_t context;
        int ch;
        int samplerate=0;

        static struct option longopts[] = {
                { "help", no_argument, NULL, 'h' },
                { "verbose", optional_argument, NULL, 'v' },
                { "length", required_argument, NULL, 'n' },
                { "samplerate", required_argument, NULL, 's' },

                { NULL, 0, NULL, 0 }
        };

        while ((ch = getopt_long(argc, argv, "hv::n:s:", longopts, NULL)) != -1)
                switch (ch) {
                case 'h':
                    show_help();
                    return 0;
                case 'n':
                    average_n = atoi(optarg);
                    break;
                case 'v':
                    verbose = atoi(optarg);
                    break;
                case 's':
                    samplerate = atoi(optarg);
                    break;
                default:
                    show_help();
                    return 1;
        }
      

        //init context
        context.probes_n = 0;
        context.max_time = 0;

        int r = process_data(&context, samplerate);
        if(r){
                fprintf(stderr, "error process_data %d\n", r);
                return 1;
        }
        dump_result(&context, samplerate);

        return 0;
}

