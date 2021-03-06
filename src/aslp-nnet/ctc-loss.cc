// aslp-nnet/ctc-loss.cc

// Copyright 2015   Yajie Miao
// Copyright 2016  ASLP (author: Binbin Zhang)
// Created on 2016-03-21

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "aslp-nnet/ctc-loss.h"
#include "aslp-cudamatrix/cu-math.h"
#include "aslp-cudamatrix/ctc-utils.h"
#include "util/edit-distance.h"

#include <sstream>
#include <iterator>

namespace kaldi {
namespace aslp_nnet {

void Ctc::Eval(const CuMatrixBase<BaseFloat> &net_out, const std::vector<int32> &label, CuMatrix<BaseFloat> *diff) {
    diff->Resize(net_out.NumRows(), net_out.NumCols());
    int32 num_frames = net_out.NumRows();
    int32 num_classes = net_out.NumCols();

    // label expansion by inserting blank (indexed by 0) at the beginning and end, 
    // and between every pair of labels
    int32 len_labels = label.size();
    int32 exp_len_labels = 2*len_labels + 1;

    label_expand_.resize(0);
    label_expand_.resize(exp_len_labels, 0);
    for (int l = 0; l < len_labels; l++) {
        KALDI_ASSERT(label[l] < net_out.NumCols());
        label_expand_[2*l+1] = label[l];
    }

    // compute in log scale
    CuMatrix<BaseFloat> log_nnet_out(net_out);
    log_nnet_out.ApplyLog();

    alpha_.Resize(num_frames, exp_len_labels, kSetZero);
    beta_.Resize(num_frames, exp_len_labels, kSetZero);
    for (int t = 0; t < num_frames; t++) {
        alpha_.ComputeCtcAlpha(log_nnet_out, t, label_expand_, false);
    }
    for (int t = (num_frames - 1); t >= 0; t--) {
        beta_.ComputeCtcBeta(log_nnet_out, t, label_expand_, false);
    }

    // compute the log-likelihood of the label sequence given the inputs logP(z|x)
    BaseFloat tmp1 = alpha_(num_frames-1, exp_len_labels-1); 
    BaseFloat tmp2 = alpha_(num_frames-1, exp_len_labels-2);
    BaseFloat pzx = tmp1 + log(1 + ExpA(tmp2 - tmp1));

    // compute the errors
    ctc_err_.Resize(num_frames, num_classes, kSetZero);
    ctc_err_.ComputeCtcError(alpha_, beta_, net_out, label_expand_, pzx);  // here should use the original ??

    // back-propagate the errors through the softmax layer
    ctc_err_.MulElements(net_out);
    CuVector<BaseFloat> row_sum(num_frames, kSetZero);
    row_sum.AddColSumMat(1.0, ctc_err_, 0.0);

    CuMatrix<BaseFloat> net_out_tmp(net_out);
    net_out_tmp.MulRowsVec(row_sum);
    diff->CopyFromMat(ctc_err_);

    diff->AddMat(-1.0, net_out_tmp);
    diff->ApplyFloor(-1.0);
    diff->ApplyCeiling(1.0);

    if (pzx < -10000) pzx = -10000;
    if (pzx > 10000) pzx = 10000;

    // update registries
    obj_ += -pzx;
    obj_progress_ += -pzx;
    sequences_progress_ += 1;
    sequences_num_ += 1;
    frames_progress_ += num_frames;
    frames_ += num_frames;

    // progressive reporting
    {
        if (sequences_progress_ >= report_step_) {
            KALDI_LOG << "Progress " << sequences_num_ << " sequences (" << frames_/(100.0 * 3600) << "Hr): "
                << " Obj(log[Pzx]) = " << obj_progress_/sequences_progress_
                << " Obj(frame) = " << obj_progress_/frames_progress_
                << " TokenAcc = " << 100.0*(1.0 - error_num_progress_/ref_num_progress_) << " %";
            // reset
            sequences_progress_ = 0;
            frames_progress_ = 0;
            obj_progress_ = 0.0;
            error_num_progress_ = 0;
            ref_num_progress_ = 0;
        }
    }

}

//void Ctc::EvalParallel(const std::vector<int32> &frame_num_utt, const CuMatrixBase<BaseFloat> &net_out,
//        std::vector< std::vector<int32> > &label, CuMatrix<BaseFloat> *diff) {

void Ctc::EvalParallel(const std::vector<std::string> &utt, 
                       const std::vector<int32> &frame_num_utt, 
                       const CuMatrixBase<BaseFloat> &net_out,
                       std::vector< std::vector<int32> > &label, 
                       CuMatrix<BaseFloat> *diff) {
    diff->Resize(net_out.NumRows(), net_out.NumCols());

    int32 num_sequence = frame_num_utt.size();  // number of sequences
    int32 num_frames = net_out.NumRows();
    KALDI_ASSERT(num_frames % num_sequence == 0);  // after padding, number of frames is a multiple of number of sequences

    int32 num_frames_per_sequence = num_frames / num_sequence;
    int32 num_classes = net_out.NumCols();
    int32 max_label_len = 0;
    for (int32 s = 0; s < num_sequence; s++) {
        if (label[s].size() > max_label_len) max_label_len = label[s].size();
    }

    // label expansion
    std::vector<int32> label_lengths_utt(num_sequence);
    int32 exp_len_labels = 2*max_label_len + 1;
    label_expand_.resize(0);
    label_expand_.resize(num_sequence * exp_len_labels, -1);
    for (int32 s = 0; s < num_sequence; s++) {
        std::vector<int32> label_s = label[s];
        label_lengths_utt[s] = 2 * label_s.size() + 1;
        for (int32 l = 0; l < label_s.size(); l++) {
            if (label_s[l] >= net_out.NumCols()) {
                KALDI_ERR << "label gt outdim " << label_s[l] << " " << net_out.NumCols();
            }
            label_expand_[s*exp_len_labels + 2*l] = 0;
            label_expand_[s*exp_len_labels + 2*l + 1] = label_s[l];
        }
        label_expand_[s*exp_len_labels + 2*label_s.size()] = 0;
    }

    // convert into the log scale
    CuMatrix<BaseFloat> log_nnet_out(net_out);
    log_nnet_out.ApplyLog();

    // do the forward and backward pass, to compute alpha and beta values
    alpha_.Resize(num_frames, exp_len_labels);
    beta_.Resize(num_frames, exp_len_labels);
    alpha_.Set(NumericLimits<BaseFloat>::log_zero_);
    beta_.Set(NumericLimits<BaseFloat>::log_zero_);
    for (int t = 0; t < num_frames_per_sequence; t++) {
        alpha_.ComputeCtcAlphaMSeq(log_nnet_out, t, label_expand_, frame_num_utt);
    }
    for (int t = (num_frames_per_sequence - 1); t >= 0; t--) {
        beta_.ComputeCtcBetaMSeq(log_nnet_out, t, label_expand_, frame_num_utt, label_lengths_utt);
    }
    CuVector<BaseFloat> pzx(num_sequence, kSetZero);
    for (int s = 0; s < num_sequence; s++) {
        int label_len = 2* label[s].size() + 1;
        int frame_num = frame_num_utt[s];
        BaseFloat tmp1 = alpha_((frame_num-1)*num_sequence + s, label_len - 1);
        BaseFloat tmp2 = alpha_((frame_num-1)*num_sequence + s, label_len-2);
        //pzx(s) = tmp1 + log(1 + ExpA(tmp2 - tmp1));
        pzx(s) = (float)LogAPlusB((double)tmp1, (double)tmp2);
    }

    // gradients from CTC
    ctc_err_.Resize(num_frames, num_classes, kSetZero);
    ctc_err_.ComputeCtcErrorMSeq(alpha_, beta_, net_out, label_expand_, frame_num_utt, pzx);  // here should use the original ??

    // back-propagate the errors through the softmax layer
    ctc_err_.MulElements(net_out);
    CuVector<BaseFloat> row_sum(num_frames, kSetZero);
    row_sum.AddColSumMat(1.0, ctc_err_, 0.0);

    CuMatrix<BaseFloat> net_out_tmp(net_out);
    net_out_tmp.MulRowsVec(row_sum);
    diff->CopyFromMat(ctc_err_);

    diff->AddMat(-1.0, net_out_tmp);
    //{
    //    Output ko("eesen.diff", false);
    //    diff->Write(ko.Stream(), false);
    //    KALDI_ERR << "Write eesen diff";
    //}

    // update registries
    pzx.Scale(-1);
    Vector<BaseFloat> pzx_host(pzx);

#if CTC_GRAD_CHECK == SUM_LOSS_CHECK
    StatAndLossCheck(utt, frame_num_utt, pzx_host, diff);
#elif CTC_GRAD_CHECK == AVG_LOSS_CHECK
    StatAndAverageLossCheck(utt, frame_num_utt, pzx_host, diff);
#else // Default stat only no check 
    StatOnly(utt, frame_num_utt, pzx_host, diff);
#endif
    // Clip diff, ensure that it is reasonable
    diff->ApplyFloor(-1.0);
    diff->ApplyCeiling(1.0);

    // progressive reporting
    {
        if (sequences_progress_ >= report_step_) {
            KALDI_LOG << "Progress " << sequences_num_ << " sequences (" << frames_/(100.0 * 3600) << "Hr):"
                << " Obj(log[Pzx]) = " << obj_progress_/sequences_progress_
                << " Obj(frame) = " << obj_progress_/frames_progress_
                << " TokenAcc = " << 100.0*(1.0 - error_num_progress_/ref_num_progress_) << " %";
            // reset
            sequences_progress_ = 0;
            frames_progress_ = 0;
            obj_progress_ = 0.0;
            error_num_progress_ = 0;
            ref_num_progress_ = 0;
        }
    }

}

void Ctc::StatAndAverageLossCheck(const std::vector<std::string> &utt, 
                                  const std::vector<int32> &frame_num_utt, 
                                  const Vector<BaseFloat> &pzx_host,
                                  CuMatrix<BaseFloat> *diff) {
    int32 num_sequence = frame_num_utt.size();  // number of sequences
    for (int s = 0; s < num_sequence; s++) {
        //if (pzx_host(s) < 0 || pzx_host(s) > 3000) {
        //    KALDI_WARN << utt[s] << " obj is abnoraml " << pzx_host(s);
        //    continue;
        //}
        // Acc enough stat for check, no check
        if (normal_num_ < stat_period_ / 2) {  
            if (KALDI_ISFINITE(pzx_host(s)) && pzx_host(s) > 0 && 
                pzx_host(s) < 3000) {
                normal_num_++;
                double loss_per_frame = pzx_host(s) / frame_num_utt[s];
                loss_sum_ += loss_per_frame;
                loss_sum_bak_ += loss_per_frame;
                loss_square_sum_ += loss_per_frame * loss_per_frame;
                loss_square_sum_bak_ += loss_per_frame * loss_per_frame;
                obj_ += pzx_host(s);
                obj_progress_ += pzx_host(s);
            }
        }
        // Check
        else {
            double loss_per_frame = pzx_host(s) / frame_num_utt[s];
            double mean = loss_sum_ / normal_num_;
            double sigma = sqrt(loss_square_sum_ / normal_num_);
            // 3sigma criterion
            if (KALDI_ISFINITE(pzx_host(s)) &&
                    (loss_per_frame >= (mean - 6 * sigma) && 
                    loss_per_frame <= (mean + 6 * sigma)) && 
                    (pzx_host(s) > 0 && pzx_host(s) < 3000)) {
                normal_num_++;
                loss_sum_ += loss_per_frame;
                loss_square_sum_ += loss_per_frame * loss_per_frame;
                obj_ += pzx_host(s);
                obj_progress_ += pzx_host(s);
                // Reset the mean and sum for new stat
                if (normal_num_ == stat_period_) {
                   loss_sum_ -= loss_sum_bak_;
                   loss_square_sum_ -= loss_square_sum_bak_;
                   loss_sum_bak_ = loss_sum_;
                   loss_square_sum_bak_ = loss_square_sum_;
                   normal_num_ = stat_period_ / 2;
                }
            } 
            else {
                // avgloss is abnormal
                KALDI_WARN << "Sequences " << utt[s]
                    << " obj is abnormal(sum " << pzx_host(s) 
                    << " per_frame " << loss_per_frame
                    << " mean " << loss_sum_ / normal_num_
                    << " sigma " << loss_square_sum_ / normal_num_ 
                    << "), drop it's diff and stat";
                for (int t = 0; t < frame_num_utt[s]; t++) {
                    diff->Row(t*num_sequence+s).SetZero();
                }
            }
        } // else

        frames_ += frame_num_utt[s];
        frames_progress_ += frame_num_utt[s];
    }
    double grad_sum = diff->Sum();
     // If some elem of diff is nan or inf, set all diff to zero for robust
    if (!KALDI_ISFINITE(grad_sum)) {
        KALDI_WARN << "DIFF FINITE: nan or inf ocurred in the diff, ignore";
        diff->SetZero();
    }
    sequences_progress_ += num_sequence;
    sequences_num_ += num_sequence;
}

void Ctc::StatAndLossCheck(const std::vector<std::string> &utt, 
                           const std::vector<int32> &frame_num_utt, 
                           const Vector<BaseFloat> &pzx_host,
                           CuMatrix<BaseFloat> *diff) {
    int32 num_sequence = frame_num_utt.size();  // number of sequences
    for (int s = 0; s < num_sequence; s++) {
        //KALDI_LOG << pzx_host(s);
        // If abnormal, drop the diff and statistic
        if (pzx_host(s) > 3000 || pzx_host(s) < 0) { 
            KALDI_WARN << "Sequences " << utt[s]
                       << " obj is abnormal(" << pzx_host(s) 
                       << "), drop it's diff and stat";
            for (int t = 0; t < frame_num_utt[s]; t++) {
                diff->Row(t*num_sequence+s).SetZero();
            }
        }
        else {
            obj_ += pzx_host(s);
            obj_progress_ += pzx_host(s);
        }
        frames_ += frame_num_utt[s];
        frames_progress_ += frame_num_utt[s];
    }
    sequences_progress_ += num_sequence;
    sequences_num_ += num_sequence;
}

void Ctc::StatOnly(const std::vector<std::string> &utt, 
        const std::vector<int32> &frame_num_utt, 
        const Vector<BaseFloat> &pzx_host,
        CuMatrix<BaseFloat> *diff) {
    int32 num_sequence = frame_num_utt.size();  // number of sequences
    for (int s = 0; s < num_sequence; s++) {
        obj_ += pzx_host(s);
        obj_progress_ += pzx_host(s);
        frames_progress_ += frame_num_utt[s];
        frames_ += frame_num_utt[s];
    }
    sequences_progress_ += num_sequence;
    sequences_num_ += num_sequence;
}

void Ctc::ErrorRate(const CuMatrixBase<BaseFloat> &net_out, const std::vector<int32> &label, float* err_rate, std::vector<int32> *hyp) {

    // frame-level labels, by selecting the label with the largest probability at each frame
    CuArray<int32> maxid(net_out.NumRows());
    net_out.FindRowMaxId(&maxid);

    int32 dim = maxid.Dim();

    std::vector<int32> data(dim);
    maxid.CopyToVec(&data);

    // remove the repetitions
    int32 i = 1, j = 1;
    while(j < dim) {
        if (data[j] != data[j-1]) {
            data[i] = data[j];
            i++;
        }
        j++;
    }
    // remove the blanks
    std::vector<int32> hyp_seq(0);
    for (int32 n = 0; n < i; n++) {
        if (data[n] != 0) {
            hyp_seq.push_back(data[n]);
        }
    }
    hyp->resize(0);
    *hyp = hyp_seq;

    int32 err, ins, del, sub;
    err =  LevenshteinEditDistance(label, hyp_seq, &ins, &del, &sub);
    *err_rate = (100.0 * err) / label.size();
    error_num_ += err;
    ref_num_ += label.size();
    error_num_progress_ += err;
    ref_num_progress_ += label.size();
}

void Ctc::ErrorRateMSeq(const std::vector<int> &frame_num_utt, const CuMatrixBase<BaseFloat> &net_out, std::vector< std::vector<int> > &label) {

    // frame-level labels
    CuArray<int32> maxid(net_out.NumRows());
    net_out.FindRowMaxId(&maxid);

    int32 dim = maxid.Dim();
    std::vector<int32> data(dim);
    maxid.CopyToVec(&data);

    // compute errors sequence by sequence
    int32 num_seq = frame_num_utt.size();
    for (int32 s = 0; s < num_seq; s++) {
        int32 num_frame = frame_num_utt[s];
        std::vector<int32> raw_hyp_seq(num_frame);
        for (int32 f = 0; f < num_frame; f++) {
            raw_hyp_seq[f] = data[f*num_seq + s];
        }    
        int32 i = 1, j = 1;
        while(j < num_frame) {
            if (raw_hyp_seq[j] != raw_hyp_seq[j-1]) {
                raw_hyp_seq[i] = raw_hyp_seq[j];
                i++;
            }
            j++;
        }
        std::vector<int32> hyp_seq(0);
        for (int32 n = 0; n < i; n++) {
            if (raw_hyp_seq[n] != 0) {
                hyp_seq.push_back(raw_hyp_seq[n]);
            }
        }
        int32 err, ins, del, sub;
        err =  LevenshteinEditDistance(label[s], hyp_seq, &ins, &del, &sub);
        error_num_ += err;
        ref_num_ += label[s].size();
        error_num_progress_ += err;
        ref_num_progress_ += label[s].size();
    }
}

std::string Ctc::Report() {
    std::ostringstream oss;
    oss << " Obj(log[Pzx]) = " << obj_/sequences_num_
        << " Obj(frame) = " << obj_/frames_ 
        << " TOKEN_ACCURACY >> " << 100.0*(1.0 - error_num_/ref_num_) << " % <<";
    return oss.str(); 
}
/* The following program was created in Baidu when I was an intern
 * Keep it here for eesen ctc has no cpu implementation
 * TODO add it
 */
/*  
double k_exp_max = std::numeric_limits<double>::max();
double k_exp_min = std::numeric_limits<double>::min();
double k_exp_limit = std::log(k_exp_max);
double k_log_infinity = 1e20;
double k_log_zero = -k_log_infinity;
static double SafeExp(double x) {
    if (x <= k_log_zero) return 0;
    if (x >= k_exp_limit) return k_exp_max;
    return std::exp(x);
}
static double SafeLog(double x) {
    if (x <= k_exp_min) return k_log_zero;
    //if (x <= 1e-100) return std::log(1e-100);
    return std::log(x);
}
static double LogAdd(double x, double y) {
    if (x <= k_log_zero) return y;
    if (y <= k_log_zero) return x;
    if (x < y) {
        double t = x;
        x = y; 
        y = t;
    }
    return x + std::log(1.0 + SafeExp(y - x));
}

#define CTC_LOG
#ifdef CTC_LOG
double CtcLoss::CtcError(const std::vector<int> &target, const Matrix<BaseFloat> &out, 
        Matrix<BaseFloat> *out_diff) {
    using namespace std;
    std::vector<int> tmp_label(target);
	//1. convert to sentence uniq label sequence, (eg. a a a b b b a a --> a b a) 
	vector<int>::iterator it = unique(tmp_label.begin(), tmp_label.end());	
	tmp_label.erase(it, tmp_label.end());
	//2. insert blank(here use sp as blank)
	RemoveBlank(&tmp_label);
	vector<int> labels(tmp_label);
	//insert blank in label sequence, (eg, a b a --> | a | b | a |
	InsertBlank(&labels);	
	KALDI_ASSERT(labels.size() == tmp_label.size() * 2 + 1);
	
	int num_frame = target.size();	
	int num_state = labels.size();
	Matrix<BaseFloat> alpha(num_frame, num_state);
    Matrix<BaseFloat> beta(num_frame, num_state);
    alpha.Set(SafeLog(0));
    beta.Set(SafeLog(0));
	//4. forward
	alpha(0,0) = SafeLog(out(0, labels[0])); //blank
	alpha(0,1) = SafeLog(out(0, labels[1])); //first label
	for (int t = 1; t < num_frame; t++) {
		int start = num_state - 2 * (num_frame - t);
        if (start < 0) start = 0;
        int end = 2 * (t + 1);
        if (end > num_state) end = num_state;
        //for (int s = 0; s < num_state; s++) {
        for (int s = start; s < end; s++) {
			double sum = alpha(t-1,s); //s
            if (s >= 1) 
                sum = LogAdd(sum, alpha(t-1, s-1)); //s-1
            //s != blank && l(s) != l(s-2)
            if (s >= 2 && s % 2 != 0 && labels[s] != labels[s-2])
                sum = LogAdd(sum, alpha(t-1,s-2));
			alpha(t,s) = sum + SafeLog(out(t,labels[s]));
        }
	}
    double like = LogAdd(alpha(num_frame-1, num_state-2), alpha(num_frame-1, num_state-1));
	//5. backward
    beta(num_frame-1, num_state-1)  = SafeLog(1.0);
	beta(num_frame-1, num_state-2)  = SafeLog(1.0);
	for (int t = num_frame - 2; t >= 0; t--) {
        int start = num_state - 2 * (num_frame - t);
        if (start < 0) start = 0;
        int end = 2 * (t + 1);
        if (end > num_state) end = num_state;
		//for (int s = num_state - 1; s >= 0; s--) {
		for (int s = end - 1; s >= start; s--) {
			double sum = beta(t+1, s) + SafeLog(out(t+1, labels[s])); //s
			if (s < num_state - 1) 
                sum = LogAdd(sum, beta(t+1, s+1) + SafeLog(out(t+1, labels[s+1])));
            if (s < num_state -2 && s % 2 != 0 && labels[s] != labels[s+2])
                sum = LogAdd(sum, beta(t+1, s+2) + SafeLog(out(t+1, labels[s+2])));
			beta(t,s) = sum;
		}
	}
    double like2 = LogAdd(beta(0,0) + SafeLog(out(0, labels[0])), beta(0,1) + SafeLog(out(0, labels[1])));
    KALDI_LOG << "alpha" << like  << ";beta " << like2 << ";num_frame " << num_frame <<  ";average " << like / num_frame;
    //6. calc out_diff
    out_diff->Resize(num_frame, out.NumCols());
    for (int t = 0; t < num_frame; t++) {
        vector<double> alpha_beta(out.NumCols(), SafeLog(0));
        for (int s = 0; s < labels.size(); s++) {
            alpha_beta[labels[s]] = LogAdd(alpha_beta[labels[s]], alpha(t, s) + beta(t, s));
        }
        for (int k = 0; k < out.NumCols(); k++) {
            //printf("alpha_beta %d %d %lf %lf\n", t, k, alpha_beta[k], SafeExp(alpha_beta[k] - like));
            double val = SafeExp(alpha_beta[k] - like);
            if (val > 1.0) val = 1.0;
		    (*out_diff)(t, k) = val - out(t,k);
        }
 	}
    return like;
}

#else
double CtcLoss::CtcError(const std::vector<int> &target, const Matrix<BaseFloat> &out, 
        Matrix<BaseFloat> *out_diff) {
    using namespace std;
    std::vector<int> tmp_label(target);
	//1. convert to sentence uniq label sequence, (eg. a a a b b b a a --> a b a) 
	vector<int>::iterator it = unique(tmp_label.begin(), tmp_label.end());	
	tmp_label.erase(it, tmp_label.end());
	//2. insert blank(here use sp as blank)
	RemoveBlank(&tmp_label);
	vector<int> labels(tmp_label);
	//insert blank in label sequence, (eg, a b a --> | a | b | a |
	InsertBlank(&labels);	
	KALDI_ASSERT(labels.size() == tmp_label.size() * 2 + 1);
	//2. get used k output in ctc, (eg. a b a b a --> a b, only label a b occured in phone sequence
	tmp_label.push_back(blank_index_);
	sort(tmp_label.begin(), tmp_label.end());		
	it = unique(tmp_label.begin(), tmp_label.end());	
	tmp_label.erase(it, tmp_label.end());	
	vector<int> used_k(tmp_label);
	
	int num_frame = target.size();	
	int num_state = labels.size();
	int num_k = used_k.size(); //used k label in total output label
	//3. k2s
	vector< vector<int> > k2s;
	k2s.resize(num_k);
	for (int k = 0; k < num_k; k++) {
		for (int s = 0; s < num_state; s++) {
			if (used_k[k] == labels[s])
				k2s[k].push_back(s);
		}
	}
	Matrix<double> alpha(num_frame, num_state);
    Matrix<double> beta(num_frame, num_state);
	vector<double> cv(num_frame, 0);
	vector<double> dv(num_frame, 0);
	//4. forward
	alpha(0,0) = out(0, labels[0]); //blank
	alpha(0,1) = out(0, labels[1]); //first label
	cv[0] = alpha(0,0) + alpha(0,1);
	alpha(0,0) = alpha(0,0)/cv[0];
  	alpha(0,1) = alpha(0,1)/cv[0];
	for (int t = 1; t < num_frame; t++) {
		int start = num_state - 2 * (num_frame - t);
        if (start < 0) start = 0;
        int end = 2 * (t + 1);
        if (end > num_state) end = num_state;
        //for (int s = start; s < end; s++) {
        for (int s = 0; s < num_state; s++) {
			double sum = alpha(t-1,s); //s
            if (s >= 1) sum += alpha(t-1,s-1); //s-1
            //s != blank && l(s) != l(s-2)
            if (s >= 2 && s % 2 == 0 && labels[s] != labels[s-2])
                sum += alpha(t-1,s-2);
			alpha(t,s) = sum * out(t,labels[s]);
		}
		//scaling
        cv[t]=0;
        for(int s = 0; s < num_state; s++) cv[t] += alpha(t,s);
        for(int s = 0; s < num_state; s++) alpha(t,s) = alpha(t,s)/cv[t];
	}
	//5. backward
    beta(num_frame-1, num_state-1)  = 1;
	beta(num_frame-1, num_state-2)  = 1;
    dv[num_frame-1] = beta(num_frame-1, num_state-1) + beta(num_frame-1, num_state-2);
	beta(num_frame-1, num_state-1) = beta(num_frame-1, num_state-1)/dv[num_frame-1];
	beta(num_frame-1, num_state-2) = beta(num_frame-1, num_state-2)/dv[num_frame-1];
	for (int t = num_frame - 2; t >= 0; t--) {
        int start = num_state - 2 * (num_frame - t);
        if (start < 0) start = 0;
        int end = 2 * (t + 1);
        if (end > num_state) end = num_state;
		//for (int s = end - 1; s >= start; s--) {
		for (int s = num_state - 1; s >= 0; s--) {
			double sum = beta(t+1, s) * out(t+1, labels[s]); //s
			if (s < num_state - 1) 
                sum += beta(t+1,s+1) * out(t+1, labels[s+1]); //s+1
            if (s < num_state -2 && s % 2 == 0 && labels[s] != labels[s+2])
				sum += beta(t+1,s+2) * out(t+1,labels[s+2]);
			beta(t,s) = sum;
		}
		//scaling
		dv[t]=0;
        for(int s = 0; s < num_state; s++) dv[t] += beta(t,s);
        for(int s = 0; s < num_state; s++) beta(t,s) = beta(t,s)/dv[t];
	}
	//6. calc out_diff
    out_diff->Resize(num_frame, out.NumCols());
	vector<double> zt(num_frame, 0);
    vector<double> ct_acc(num_frame, 0); 
    vector<double> dt_acc(num_frame, 0); 
    ct_acc[0] = log(cv[0]);
	for (int t = 1; t < num_frame; t++)
		ct_acc[t] = ct_acc[t-1] + log(cv[t]);
    dt_acc[num_frame-1] = log(dv[num_frame-1]);
	for (int t = num_frame-2; t>=0; t--)
		dt_acc[t] = dt_acc[t+1] + log(dv[t]);
	for (int t = 0; t < num_frame; t++) {
		for (int j=0; j <out_diff->NumCols(); j++){
			(*out_diff)(t,j) = 0 - out(t,j);
		}
		for (int s = 0; s < num_state; s++) {
			zt[t] += alpha(t,s) * beta(t,s);
            printf("t=%d s=%d alpha(t,s)=%lf beta(t,s)=%lf mul=%lf zt=%lf\n", t, s, alpha(t,s), beta(t,s), alpha(t,s)*beta(t,s), zt[t]);
		}
		//cout<<"t="<<t<<": z="<<log(zt[t])+ct_acc[t]+dt_acc[t]<<endl;
		for (int k = 0; k < num_k; k++) {
			double alpha_beta = 0;
			for (int i = 0; i < k2s[k].size(); i++) {
				alpha_beta += alpha(t, k2s[k][i]) * beta(t, k2s[k][i]);
			}
            KALDI_LOG << alpha_beta << " " << zt[t];
            KALDI_ASSERT(zt[t] >= 0);
            KALDI_ASSERT(alpha_beta <= zt[t]);
			if (zt[t] > 0){
				//tmpDiff.data_[t*num_frame+k] = out(t,used_k[k]) - 1.0/zt[t] * alpha_beta;
				 (*out_diff)(t, used_k[k]) = alpha_beta/zt[t] - out(t,used_k[k]);
			}
		}
 	}
	return (log(zt[0])+ct_acc[0]+dt_acc[0]);//average log likelihood of each frame
}
#endif
*/

} // namespace aslp_nnet
} // namespace kaldi
