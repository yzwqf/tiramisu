#include <tiramisu/tiramisu.h>

#include "configuration.h"

#define GEMM_BATCH 10

using namespace tiramisu;

int main(int argc, char **argv)
{
    // Single LSTM block without minibatching
    tiramisu::init("lstm");

    // -------------------------------------------------------
    // Layer I
    // -------------------------------------------------------

    // Inner dimensions
    var i("i", 0, FEATURE_SIZE), j("j", 0, FEATURE_SIZE), k("k", 0, BATCH_SIZE);
    var i_merged("i_merged", 0, 4 * FEATURE_SIZE);
    var i0("i0"), i1("i1"), k0("k0"), k1("k1");
    // Outer dimensions
    var l("l", 0, NUM_LAYERS), s("s", 0, SEQ_LENGTH);
    var s0("s0", 0, SEQ_LENGTH / GEMM_BATCH), s1("s1", 0, GEMM_BATCH);
    // After skewing
    var l_s("l_s"), s_s("s_s");

    input x("x", {s, k, i}, p_float32);
    input weights("weights", {l, var("w_i", 0, 2), i_merged, j}, p_float32);
    input biases("biases", {l, i_merged}, p_float32);
    input tmp("tmp", {s, k, i_merged}, p_float32);
    weights.get_buffer()->tag_gpu_global();
    biases.get_buffer()->tag_gpu_global();
    tmp.get_buffer()->tag_gpu_global();

    buffer buf_Weights_cpu("buf_Weights_cpu", {NUM_LAYERS, 2, 4 * FEATURE_SIZE, FEATURE_SIZE}, p_float32, a_input);
    buffer buf_h("buf_h", {NUM_LAYERS + 1, SEQ_LENGTH + 1, BATCH_SIZE, FEATURE_SIZE}, p_float32, a_temporary);
    buffer buf_biases_cpu("buf_biases_cpu", {NUM_LAYERS, 4 * FEATURE_SIZE}, p_float32, a_input);
    buffer buf_x_cpu("buf_x_cpu", {SEQ_LENGTH, BATCH_SIZE, FEATURE_SIZE}, p_float32, a_input);
    buffer buf_y_cpu("buf_y_cpu", {SEQ_LENGTH, BATCH_SIZE, FEATURE_SIZE}, p_float32, a_output);
    buffer buf_x("buf_x", {SEQ_LENGTH, BATCH_SIZE, FEATURE_SIZE}, p_float32, a_temporary);
    buffer buf_y("buf_y", {SEQ_LENGTH, BATCH_SIZE, FEATURE_SIZE}, p_float32, a_temporary);
    buffer buf_tmp_i("buf_tmp_i", {NUM_LAYERS, BATCH_SIZE, FEATURE_SIZE}, p_float32, a_temporary);
    buffer buf_tmp_z("buf_tmp_z", {NUM_LAYERS, BATCH_SIZE, FEATURE_SIZE}, p_float32, a_temporary);
    buffer buf_tmp_o("buf_tmp_o", {NUM_LAYERS, BATCH_SIZE, FEATURE_SIZE}, p_float32, a_temporary);
    buffer buf_tmp_f("buf_tmp_f", {NUM_LAYERS, BATCH_SIZE, FEATURE_SIZE}, p_float32, a_temporary);
    buffer buf_c("buf_c", {NUM_LAYERS, SEQ_LENGTH + 1, BATCH_SIZE, FEATURE_SIZE}, p_float32, a_temporary);
    buf_h.tag_gpu_global();
    buf_x.tag_gpu_global();
    buf_y.tag_gpu_global();
    buf_tmp_i.tag_gpu_global();
    buf_tmp_z.tag_gpu_global();
    buf_tmp_o.tag_gpu_global();
    buf_tmp_f.tag_gpu_global();
    buf_c.tag_gpu_global();

    // Transpose Weights
    var w_t_i("w_i", 0, 2);  // Dummy variable
    computation weights_T({l, w_t_i, j, i_merged}, weights(l, w_t_i, i_merged, j));
    weights_T.get_buffer()->tag_gpu_global();
    // h(l, s) is the output of the block (l, s)
    // which takes h(l, s - 1) and h(l - 1, s) as inputs
    // initial hidden states are h(l, -1) and c(l, -1)
    // input x is copied to h(-1, s)
    computation h({l, s, k, i}, p_float32);
    computation c({l, s, k, i}, p_float32);
    computation h_init({l, k, i}, expr(float(0)));
    computation c_init({l, k, i}, expr(float(0)));
    computation h_copy_x({s, k, i}, x(s, k, i));
    // Multiplication from input is batched:
    computation sum1({l, s0},
        cublas_sgemm(buf_h, *weights_T.get_buffer(), *tmp.get_buffer(),
                     GEMM_BATCH * BATCH_SIZE, 4 * FEATURE_SIZE, FEATURE_SIZE,
                     1, 0,  // alpha, beta
                     0, 0, 0,  // ldABC
                     (l * (SEQ_LENGTH + 1) + s0 * GEMM_BATCH + 1) * BATCH_SIZE * FEATURE_SIZE,  //offsetA
                     (l * 2 + 1) * 4 * FEATURE_SIZE * FEATURE_SIZE,  //offsetB
                     s0 * GEMM_BATCH * BATCH_SIZE * 4 * FEATURE_SIZE,  // offsetC
                     false, false));
    computation sum2({l, s},
        cublas_sgemm(buf_h, *weights_T.get_buffer(), *tmp.get_buffer(),
                     BATCH_SIZE, 4 * FEATURE_SIZE, FEATURE_SIZE,
                     1, 1,  // alpha, beta
                     0, 0, 0,  // ldABC
                     ((l + 1) * (SEQ_LENGTH + 1) + s) * BATCH_SIZE * FEATURE_SIZE,  //offsetA
                     (l * 2) * 4 * FEATURE_SIZE * FEATURE_SIZE,  //offsetB
                     s * BATCH_SIZE * 4 * FEATURE_SIZE,  // offsetC
                     false, false));
    #define sigmoid(x) expr(float(1)) / (1 + expr(o_expo, -(x)))
    computation sig_i({l, s, k, i},      sigmoid(tmp(s, k, i + 0 * FEATURE_SIZE) + biases(l, i + 0 * FEATURE_SIZE)));
    computation tnh_z({l, s, k, i}, expr(o_tanh, tmp(s, k, i + 1 * FEATURE_SIZE) + biases(l, i + 1 * FEATURE_SIZE)));
    computation sig_o({l, s, k, i},      sigmoid(tmp(s, k, i + 2 * FEATURE_SIZE) + biases(l, i + 2 * FEATURE_SIZE)));
    computation sig_f({l, s, k, i},      sigmoid(tmp(s, k, i + 3 * FEATURE_SIZE) + biases(l, i + 3 * FEATURE_SIZE)));
    computation mul_iz({l, s, k, i}, sig_i(l, s, k, i) * tnh_z(l, s, k, i));
    computation mul_fc({l, s, k, i}, sig_f(l, s, k, i) * c(l, s - 1, k, i));
    c.set_expression(mul_iz(l, s, k, i) + mul_fc(l, s, k, i));
    computation tnh_c({l, s, k, i}, expr(o_tanh, c(l, s, k, i)));
    h.set_expression(tnh_c(l, s, k, i) * sig_o(l, s, k, i));
    computation stream_sync({l, s0}, cuda_stream_synchronize());
    // Output is the last layer
    computation y({s, k, i}, h(NUM_LAYERS - 1, s, k, i));
    // Copies
    computation copy_Weights_to_device({}, memcpy(buf_Weights_cpu, *weights.get_buffer()));
    computation copy_biases_to_device({}, memcpy(buf_biases_cpu, *biases.get_buffer()));
    computation copy_x_to_device({}, memcpy(buf_x_cpu, buf_x));
    computation copy_y_to_host({}, memcpy(buf_y, buf_y_cpu));

    // -------------------------------------------------------
    // Layer II
    // -------------------------------------------------------

    // Fuse kernels by moving gpu iterators out
    weights_T.interchange(w_t_i, j);
    weights_T.interchange(w_t_i, i_merged);
    weights_T.interchange(l, j);
    weights_T.interchange(l, i_merged);
    h_init.interchange(l, k);
    h_init.interchange(l, i);
    c_init.interchange(l, k);
    c_init.interchange(l, i);
    h_copy_x.interchange(s, k);
    h_copy_x.interchange(s, i);
    y.interchange(s, k);
    y.interchange(s, i);

    weights_T.gpu_tile(j, i_merged, 16, 16);
    block nonlinear_block({&sig_i, &tnh_z, &sig_o, &sig_f, &mul_iz, &mul_fc, &c,
                           &tnh_c, &h});
    // Batch Input GEMMs
    block({&sum2, &nonlinear_block}).split(s, GEMM_BATCH, s0, s1);
    block ki_block({&h_init, &c_init, &h_copy_x, &nonlinear_block, &y});
    // Interchange to get better locality
    ki_block.interchange(k, i);
    ki_block.gpu_tile(i, k, 16, 16, i0, k0, i1, k1);
    block lstm_block({&sum1, &sum2, &nonlinear_block, &stream_sync});
    // Skew and interchange to get diagonal traversal
    lstm_block.skew(l, s0, 1, l_s, s_s);
    lstm_block.interchange(l_s, s_s);
    // Parallelize diagonal traversal
    // Due to a bug in tagging system we only need to parallelize one computation
    sum1.parallelize(l_s);

    // Scheduling commands
    copy_Weights_to_device
            .then(copy_biases_to_device, computation::root)
            .then(copy_x_to_device, computation::root)
            .then(weights_T, computation::root)
            .then(h_init, computation::root)
            .then(c_init, computation::root)
            .then(h_copy_x, computation::root)
            .then(sum1, computation::root)
            .then(sum2, l_s)
            .then(sig_i, s1)
            .then(tnh_z, k1)
            .then(sig_o, k1)
            .then(sig_f, k1)
            .then(mul_iz, k1)
            .then(mul_fc, k1)
            .then(c, k1)
            .then(tnh_c, k1)
            .then(h, k1)
            .then(stream_sync, l_s)
            .then(y, computation::root)
            .then(copy_y_to_host, computation::root);

    // -------------------------------------------------------
    // Layer III
    // -------------------------------------------------------

    // Weights and biases are packed
    x.store_in(&buf_x);
    y.store_in(&buf_y);
    sig_i.store_in(&buf_tmp_i, {l, k, i});
    tnh_z.store_in(&buf_tmp_z, {l, k, i});
    sig_o.store_in(&buf_tmp_o, {l, k, i});
    sig_f.store_in(&buf_tmp_f, {l, k, i});
    mul_iz.store_in(&buf_tmp_i, {l, k, i});
    mul_fc.store_in(&buf_tmp_f, {l, k, i});
    tnh_c.store_in(&buf_tmp_i, {l, k, i});
    h.store_in(&buf_h, {l + 1, s + 1, k, i});
    c.store_in(&buf_c, {l, s + 1, k, i});
    h_init.store_in(&buf_h, {l + 1, 0, k, i});
    c_init.store_in(&buf_c, {l, 0, k, i});
    h_copy_x.store_in(&buf_h, {0, s + 1, k, i});

    // -------------------------------------------------------
    // Code Generation
    // -------------------------------------------------------

    // Generate object files.
    tiramisu::codegen({
            &buf_Weights_cpu,
            &buf_biases_cpu,
            &buf_x_cpu,
            &buf_y_cpu,
        }, "lstm.o", true);

    return 0;
}
