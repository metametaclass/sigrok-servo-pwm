#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>

#include <time.h>
#include <limits.h>
#include <getopt.h>
#include <errno.h>

#define BUFFER_SIZE 160
#define MAX_PROBES 16
#define AVERAGER_SIZE 400

//window size for averaging
int average_n = 10;//0.025s * 400 Hz

int verbose = 0;
FILE *dump_file = NULL;

#define DETAIL 10

//average calculation struct
//uses circular buffer and calculates average, median and root-square-mean deviation
typedef struct average_data {
        int data_count;
        int circular_index;
        int buffer[AVERAGER_SIZE];

        double average;
        double rmsd;

        int last_value;
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

        avg->last_value = value;
        avg->buffer[avg->circular_index] = value;
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

        bool is_sbus_mode;
        int is_sbus_active;
        int sbus_start_time;
        int sbus_bit_counter;
        int start_bit_count;
        int sbus_errors;
        int sbus_bytes;
        uint16_t sbus_bits;

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
        int bit_interval;
        int max_time;
        int line_num;
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


void feed_bit(context_t *ctx, int probe_idx, probe_data_t *data, int value){

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
                 if((dump_file!=NULL) && probe_has_enough_data(data)){
                         fprintf(dump_file, "%d,%d,%d,%d,%d,%d,%d,%f,%f,%f,%f\n",
                                 probe_idx, ctx->line_num, data->time,
                                 data->pulse_width_avg.last_value, data->period_avg.last_value,
                                 data->pulse_width_avg.median, data->period_avg.median,
                                 data->pulse_width_avg.average, data->period_avg.average,
                                 data->pulse_width_avg.rmsd, data->period_avg.rmsd);
                 }
         }

         data->last_value = value;
         data->time++;

}

void check_sbus_byte(context_t *ctx, int probe_idx, probe_data_t *data){
        (void) ctx;
        if((dump_file!=NULL)){
                fprintf(dump_file, "t:%8.8x p:%d v:%4.4x\n", data->time, probe_idx, data->sbus_bits);
        }
        data->sbus_bytes++;
}

void process_sbus_bit(context_t *ctx, int probe_idx, probe_data_t *data, int value){
        int shift = data->time - data->sbus_start_time;
        if(shift<ctx->bit_interval) {
            if(value){
                data->start_bit_count++;
            }
            return;
        }
        //printf("%d count start %d\n", probe_idx, data->start_bit_count);
        if(data->start_bit_count < ctx->bit_interval/2){
            data->is_sbus_active = 0;
            data->sbus_errors++;
            return;
        }

        if (((shift + ctx->bit_interval/2) % ctx->bit_interval) == 0) {
           //sample bit
            //printf("sample at %8.8x\n", data->time);
            data->sbus_bits <<= 1;
            if(value) {
                 data->sbus_bits |= 1;
            }
            data->sbus_bit_counter++;           
            if(data->sbus_bit_counter>=11){
               check_sbus_byte(ctx, probe_idx, data);
               data->is_sbus_active = 0;
            }
        }

}


void feed_bit_sbus(context_t *ctx, int probe_idx, probe_data_t *data, int value){
        if(data->is_sbus_active) {
                //printf("%d sbus_bit %d\n", probe_idx, value);
                process_sbus_bit(ctx, probe_idx, data, value);
        } else {
                if(data->last_value==0 && value!=0){
                        //printf("%d rise at %8.8x\n", probe_idx, data->time);
                        //rising edge
                        data->is_sbus_active = 1;//start sbus decode
                        data->sbus_start_time = data->time;
                        data->sbus_bits = 0;
                        data->sbus_bit_counter = 0;
                        data->start_bit_count = 0;
                }
        }
        data->last_value = value;
        data->time++;

}


int init_probes(context_t *ctx, int probe_idx, bool sbus_mode){
        if(probe_idx<0 || probe_idx>=MAX_PROBES){
                return 1;
        }

        while(ctx->probes_n<=probe_idx){
                probe_data_t *probe = &ctx->probes[ctx->probes_n];
                probe->time = 0;
                probe->last_value = 0;
                probe->rising_edge_time = -1;
                probe->falling_edge_time = -1;
                probe->pulse_count = 0;
                probe->is_sbus_mode = sbus_mode;
                probe->is_sbus_active = 0;
                probe->sbus_bit_counter = 0;
                probe->sbus_start_time = 0;
                probe->start_bit_count = 0;
                probe->sbus_bits = 0;
                probe->sbus_errors = 0;
                init_average(&probe->period_avg);
                init_average(&probe->pulse_width_avg);
                ctx->probes_n++;
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

//dump brief data averaging result
void dump_average_brief(average_data_t *avg, int samplerate){
        if(samplerate>0) {
                double koeff = 1000.0/samplerate;
                printf("l:%4.0f a:%4.0f m:%4.0f d:%4.0f \n", avg->last_value*koeff, avg->average*koeff, avg->median*koeff, avg->rmsd*koeff);
        }else{
                printf("l:%4d a:%4.1f m:%4d d:%4.1f \n", avg->last_value, avg->average, avg->median, avg->rmsd);
        }
}


//dump probe averages
void dump_result(context_t *ctx, int samplerate, bool brief){
        for(int i=0;i<ctx->probes_n;i++){
                probe_data_t *probe = &ctx->probes[i];
                printf("p: %d ", i);
                if(!probe_has_enough_data(probe)){
                        printf("no data\n");
                        continue;
                }
                if(brief){
                        printf("c:%d ", probe->pulse_count);
                        dump_average_brief(&probe->pulse_width_avg, samplerate);
                        //dump_average_brief(&probe->period_avg, samplerate);
                        continue;
                }
                printf("pulses:%d\n", probe->pulse_count);
                dump_average("  width:  ", &probe->pulse_width_avg, samplerate);
                dump_average("  period: ", &probe->period_avg, samplerate);
        }
}


//dump probe sbus data
void dump_result_sbus(context_t *ctx){
        for(int i=0;i<ctx->probes_n;i++){
                probe_data_t *probe = &ctx->probes[i];
                printf("p:%d errors:%d bytes:%d\n", i, probe->sbus_errors, probe->sbus_bytes);
        }
}


//process incoming data
int process_data_binary(context_t *ctx, int samplerate, bool sbus_mode){

        struct timespec lts;
        if(clock_gettime(CLOCK_MONOTONIC, &lts)!=0){
                fprintf(stderr, "error get clock %s\n", strerror(errno));
                return 1;
        }
        init_probes(ctx, 7, sbus_mode);//0-7 probes
        int i;
        while((i=fgetc(stdin))!=EOF){
                //printf("%p %s\n", buffer, buffer);
                int mask = 1;
                for(int probe_idx=0;probe_idx<8;probe_idx++){
                        if(sbus_mode) {
                                feed_bit_sbus(ctx, probe_idx, &ctx->probes[probe_idx], i & mask);
                        } else {
                                feed_bit(ctx, probe_idx, &ctx->probes[probe_idx], i & mask);
                        }

                        mask<<=1;
                }

                if((ctx->line_num & 0xFFF)==0){
                        struct timespec ts;
                        if(clock_gettime(CLOCK_MONOTONIC, &ts)!=0){
                                fprintf(stderr, "error get clock %s\n", strerror(errno));
                                return 1;
                        }
                        if(diffts(lts, ts)>250){
                                printf("\x1B[1;1H");
                                printf("%d\n", ctx->line_num);
                                if(sbus_mode){
                                        dump_result_sbus(ctx);
                                } else {
                                        dump_result(ctx, samplerate, true);
                                }
                                lts=ts;
                        }
                }

                ctx->line_num++;
        }
        return 0;
}


//usage help
void show_help(){
        printf("pwm (servo) signal analyzer, for using with sigrok logic analyzer software\n");
        printf(" Usage: pwm [-n buffer_length] [-s sample_rate_khz] [-v debug_level] [--sbus] [-d data_dump_file] [-h] < sigrok_binary_file\n");
        printf(" Usage: sigrok-cli -d fx2lafw --config samplerate=100k --continuous -p 0,1,2,3,4,5 -o /dev/stdout -O binary | ./pwm [-s 100] -d values.csv\n");
}


//main entry point
int main(int argc, char** argv){
        context_t context;
        int ch;
        int samplerate=0;
        bool sbus_mode = false;//frsky sbus decoder

        static struct option longopts[] = {
                { "help", no_argument, NULL, 'h' },
                { "verbose", required_argument, NULL, 'v' },
                { "length", required_argument, NULL, 'n' },
                { "samplerate", required_argument, NULL, 's' },
                { "dump", required_argument, NULL, 'd' },
                { "sbus", optional_argument, NULL, 'b' },

                { NULL, 0, NULL, 0 }
        };

        while ((ch = getopt_long(argc, argv, "hv:n:s:d:b", longopts, NULL)) != -1)
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
                case 'd':
                    dump_file = fopen(optarg, "w");
                    break;
                case 'b':
                    sbus_mode = true;
                    break;
                default:
                    show_help();
                    return 1;
        }

        //init context
        context.probes_n = 0;
        context.max_time = 0;
        context.line_num = 1;
        context.bit_interval = samplerate/100;//sbus has 100000 bit per second
        if(sbus_mode && context.bit_interval<10){
                fprintf(stderr, "sbus decoder need at least 1000k samplerate\n");
        }

        int r = 1;
        r = process_data_binary(&context, samplerate, sbus_mode);
        if(r){
                fprintf(stderr, "error process_data %d\n", r);
                return 1;
        }

        if(dump_file) {
                fclose(dump_file);
        }
        if(sbus_mode){
                dump_result_sbus(&context);
        } else {
                dump_result(&context, samplerate, false);
        }

        return 0;
}

