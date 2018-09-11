#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <unistd.h>
#include <pocketsphinx.h>

#define SEG_SIZE 160 // 10ms worth of samples
#define SEG_NUM 100000 // Totally buffer of 16M Samples
#define SAMPLE_RATE 16000
#define LP_PARA 0.0001

int stoprunning = 0;
short sample[SEG_SIZE * SEG_NUM];
int capcount = 0; // number of segments already captured. Captured segments wrap around in sample[]
double level = 0; // low-pass filtered recent sound level

// Sample from http://equalarea.com/paul/alsa-audio.html#captureex
// ALSA API: http://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m___h_w___params.html
//           http://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html

// No return value
void *capture_thread(void *ptr)
{
        int err;
        snd_pcm_t *capture_handle = NULL;
        snd_pcm_hw_params_t *hw_params;
        unsigned int samplerate = SAMPLE_RATE;
        char *micName = (char *)ptr;

        if ((err = snd_pcm_open (&capture_handle, micName, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
                fprintf (stderr, "cannot open audio device %s (%s)\n",
                         micName,
                         snd_strerror (err));
                goto HandleError;
        }

        if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) {
                fprintf (stderr, "cannot allocate hardware parameter structure (%s)\n",
                         snd_strerror (err));
                goto HandleError;
        }

        if ((err = snd_pcm_hw_params_any (capture_handle, hw_params)) < 0) {
                fprintf (stderr, "cannot initialize hardware parameter structure (%s)\n",
                         snd_strerror (err));
                goto HandleError;
        }

        if ((err = snd_pcm_hw_params_set_access (capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
                fprintf (stderr, "cannot set access type (%s)\n",
                         snd_strerror (err));
                goto HandleError;
        }

        // Signed 16bit PCM, little endian
        if ((err = snd_pcm_hw_params_set_format (capture_handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
                fprintf (stderr, "cannot set sample format (%s)\n",
                         snd_strerror (err));
                goto HandleError;
        }

        if ((err = snd_pcm_hw_params_set_rate_near (capture_handle, hw_params, &samplerate, 0)) < 0) {
                fprintf (stderr, "cannot set sample rate (%s)\n",
                         snd_strerror (err));
                goto HandleError;
        }

        if ((err = snd_pcm_hw_params_set_channels (capture_handle, hw_params, 1)) < 0) {
                fprintf (stderr, "cannot set channel count (%s)\n",
                         snd_strerror (err));
                goto HandleError;
        }

        if ((err = snd_pcm_hw_params (capture_handle, hw_params)) < 0) {
                fprintf (stderr, "cannot set parameters (%s)\n",
                         snd_strerror (err));
                goto HandleError;
        }

        snd_pcm_hw_params_free (hw_params);

        if ((err = snd_pcm_prepare (capture_handle)) < 0) {
                fprintf (stderr, "cannot prepare audio interface for use (%s)\n",
                         snd_strerror (err));
                goto HandleError;
        }

        while (!stoprunning) {
                int i;
                short *s = sample + SEG_SIZE * (capcount % SEG_NUM);
                err = snd_pcm_readi (capture_handle, s, SEG_SIZE);
                if (err != SEG_SIZE) {
                        fprintf (stderr, "read from audio interface failed (%s)\n",
                                 snd_strerror (err));
                        goto HandleError;
                }
                capcount ++;
                for (i=0; i<SEG_SIZE; i++) {
                        level = level + ((s[i]>0? s[i] : (-s[i])) - level) * LP_PARA;
                }
                if (capcount && (capcount %100)==0) {
                        // fprintf(stderr, "Captured %d segments leve=%lf\n", capcount, level);
                }
        }

        HandleError:
        if (capture_handle != NULL) snd_pcm_close(capture_handle);
        return NULL;
}


int main(int argc, char *argv[])
{
        int i;
        ps_decoder_t *ps;
        cmd_ln_t *config;
        FILE *fh;
        char const *hyp, *uttid;
        int16 buf[512];
        int rv;
        int32 score;
        char *micName = "default";
        int idx = 0;

        pthread_t cthread, pthread; // thread for capture and playback
        int cret, pret;

        if (argc>=2) micName = argv[1];

        config = cmd_ln_init(NULL, ps_args(), TRUE,
                             "-hmm", "/home/steven/lungacoustics/model_parameters/lungacoustics.cd_cont_200",
                             "-lm", "/home/steven/lungacoustics/etc/lungacoustics.lm.DMP",
                             "-dict", "/home/steven/lungacoustics/etc/lungacoustics.dic",
                             "-logfn", "/dev/null",
                             NULL);
        if (config == NULL)
                return 1;
        ps = ps_init(config);
        if (ps == NULL)
                return 1;

        cret = pthread_create(&cthread, NULL, capture_thread, (void *)micName);

        for (;;) {

#define UTT_SEC 10 // 5 sec for each utterance
#define UTT_SEGS ((UTT_SEC * SAMPLE_RATE) / SEG_SIZE)
#define UTT_NSAMPLE (UTT_SEGS * SEG_SIZE)

                static short utt[UTT_NSAMPLE]; // static, to avoid using stack space

                fprintf(stderr, "\nGetting %d sec of utterance...level=%lf\n", UTT_SEC, level);
                for (i = 0; i < UTT_SEGS; i++) {
                        short *s = sample + SEG_SIZE * (idx % SEG_NUM);
                        while (idx >= capcount) usleep(10000);
                        memcpy(utt + (i * SEG_SIZE), s, SEG_SIZE * sizeof(short));
                        idx ++;
                }
                fprintf(stderr, "Decoding the utterance...\n");
                rv = ps_start_utt(ps, micName);
                if (rv < 0) {
                        fprintf(stderr, "Failed ps_start_utt(), quit...\n");
                        goto AllDone;
                }
                rv = ps_process_raw(ps, utt, UTT_NSAMPLE, FALSE, TRUE);
                rv = ps_end_utt(ps);
                if (rv < 0) {
                        fprintf(stderr, "Failed ps_end_utt(), quit...\n");
                        goto AllDone;
                }
                hyp = ps_get_hyp(ps, &score, &uttid);
                if (hyp == NULL) {
                        printf("Utterance not recognized.\n");
                }
                else {
                        printf("Utterance (score=%d): %s\n", score, hyp);
                }
        }

        AllDone:

        fclose(fh);
        ps_free(ps);
        return 0;
}
