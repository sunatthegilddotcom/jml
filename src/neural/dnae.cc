/* dnae.cc
   Jeremy Barnes, 4 November 2009
   Copyright (c) 2009 Jeremy Barnes.  All rights reserved.

   Denoising Auto Encoder functions.
*/

#include "dnae.h"

namespace ML {



struct Test_Stack_Job {

    const DNAE_Stack & stack;
    const vector<distribution<float> > & data;
    int first;
    int last;
    float prob_cleared;
    const Thread_Context & context;
    int random_seed;
    Lock & update_lock;
    double & error_exact;
    double & error_noisy;
    boost::progress_display * progress;
    int verbosity;

    Test_Stack_Job(const DNAE_Stack & stack,
                   const vector<distribution<float> > & data,
                   int first, int last,
                   float prob_cleared,
                   const Thread_Context & context,
                   int random_seed,
                   Lock & update_lock,
                   double & error_exact,
                   double & error_noisy,
                   boost::progress_display * progress, int verbosity)
        : stack(stack), data(data),
          first(first), last(last),
          prob_cleared(prob_cleared),
          context(context), random_seed(random_seed),
          update_lock(update_lock),
          error_exact(error_exact), error_noisy(error_noisy),
          progress(progress), verbosity(verbosity)
    {
    }

    void operator () ()
    {
        Thread_Context thread_context(context);
        thread_context.seed(random_seed);

        double test_error_exact = 0.0, test_error_noisy = 0.0;

        for (unsigned x = first;  x < last;  ++x) {

            distribution<CFloat> input(data[x]);

            // Add noise
            distribution<CFloat> noisy_input
                = add_noise(input, thread_context, prob_cleared);
            
            distribution<CFloat>
                rep = stack.apply(input),
                noisy_rep = stack.apply(noisy_input);

            distribution<CFloat>
                output = stack.iapply(rep),
                noisy_output = stack.iapply(noisy_rep);

            // Error signal
            distribution<CFloat> diff
                = input - noisy_output;
    
            // Overall error
            double error = pow(diff.two_norm(), 2);

            test_error_noisy += error;

            // Error signal
            distribution<CFloat> diff2
                = input - output;
    
            // Overall error
            double error2 = pow(diff2.two_norm(), 2);
    
            test_error_exact += error2;

            if (x < 5) {
                Guard guard(update_lock);
                cerr << "ex " << x << " error " << error2 << endl;
                cerr << "    input " << input << endl;
                cerr << "    rep   " << rep << endl;
                cerr << "    out   " << output << endl;
                cerr << "    diff  " << diff2 << endl;
                cerr << endl;
            }

        }

        Guard guard(update_lock);
        error_exact += test_error_exact;
        error_noisy += test_error_noisy;
        if (progress && verbosity >= 3) (*progress) += (last - first);
    }
};

pair<double, double>
test_dnae(const LayerStackT<Twoway_Layer> & layers,
          const vector<distribution<float> > & data,
          float prob_cleared,
          Thread_Context & thread_context,
          int verbosity) const
{
    Lock update_lock;
    double error_exact = 0.0;
    double error_noisy = 0.0;

    int nx = data.size();

    std::auto_ptr<boost::progress_display> progress;
    if (verbosity >= 3) progress.reset(new boost::progress_display(nx, cerr));

    Worker_Task & worker = thread_context.worker();
            
    // Now, submit it as jobs to the worker task to be done
    // multithreaded
    int group;
    {
        int parent = -1;  // no parent group
        group = worker.get_group(NO_JOB, "dump user results task",
                                 parent);
        
        // Make sure the group gets unlocked once we've populated
        // everything
        Call_Guard guard(boost::bind(&Worker_Task::unlock_group,
                                     boost::ref(worker),
                                     group));
        
        // 20 jobs per CPU
        int batch_size = nx / (num_cpus() * 20);
        
        for (unsigned x = 0; x < nx;  x += batch_size) {
            
            Test_Stack_Job job(*this, data,
                               x, min<int>(x + batch_size, nx),
                               prob_cleared,
                               thread_context,
                               thread_context.random(),
                               update_lock,
                               error_exact, error_noisy,
                               progress.get(),
                               verbosity);
            
            // Send it to a thread to be processed
            worker.add(job, "blend job", group);
        }
    }
    
    worker.run_until_finished(group);
    
    return make_pair(sqrt(error_exact / nx),
                     sqrt(error_noisy / nx));
}

void
train_dnae(Layer_Stack<Twoway_Layer> & stack,
           const std::vector<distribution<float> > & training_data,
           const std::vector<distribution<float> > & testing_data,
           const Configuration & config,
           Thread_Context & thread_context)
{
    double learning_rate = 0.75;
    int minibatch_size = 512;
    int niter = 50;

    /// Probability that each input is cleared
    float prob_cleared = 0.10;

    int verbosity = 2;

    Transfer_Function_Type transfer_function = TF_TANH;

    bool init_with_svd = false;
    bool use_dense_missing = true;

    bool randomize_order = true;

    float sample_proportion = 0.8;

    int test_every = 1;

    vector<int> layer_sizes
        = boost::assign::list_of<int>(250)(150)(100)(50);
    
    config.get(prob_cleared, "prob_cleared");
    config.get(learning_rate, "learning_rate");
    config.get(minibatch_size, "minibatch_size");
    config.get(niter, "niter");
    config.get(verbosity, "verbosity");
    config.get(transfer_function, "transfer_function");
    config.get(init_with_svd, "init_with_svd");
    config.get(use_dense_missing, "use_dense_missing");
    config.get(layer_sizes, "layer_sizes");
    config.get(randomize_order, "randomize_order");
    config.get(sample_proportion, "sample_proportion");
    config.get(test_every, "test_every");

    int nx = training_data.size();
    int nxt = testing_data.size();

    if (nx == 0)
        throw Exception("can't train on no data");

    int nlayers = layer_sizes.size();

    vector<distribution<float> > layer_train = training_data;
    vector<distribution<float> > layer_test = testing_data;

    // Learning rate is per-example
    learning_rate /= nx;

    // Compensate for the example proportion
    learning_rate /= sample_proportion;

    for (unsigned layer_num = 0;  layer_num < nlayers;  ++layer_num) {
        cerr << endl << endl << endl << "--------- LAYER " << layer_num
             << " ---------" << endl << endl;

        vector<distribution<float> > next_layer_train, next_layer_test;

        int ni
            = layer_num == 0
            ? training_data[0].size()
            : layer_sizes[layer_num - 1];

        if (ni != layer_train[0].size())
            throw Exception("ni is wrong");

        int nh = layer_sizes[layer_num];

        Twoway_Layer layer(use_dense_missing, ni, nh, transfer_function,
                           thread_context);
        distribution<CFloat> cleared_values(ni);

        if (ni == nh && false) {
            //layer.zero_fill();
            for (unsigned i = 0;  i < ni;  ++i) {
                layer.weights[i][i] += 1.0;
            }
        }


        if (init_with_svd) {
            // Initialize with a SVD
            SVD_Decomposition init;
            init.train(layer_train, nh);
            
            for (unsigned i = 0;  i < ni;  ++i) {
                distribution<CFloat> init_i(&init.lvectors[i][0],
                                            &init.lvectors[i][0] + nh);
                //init_i /= sqrt(init.singular_values_order);
                //init_i *= init.singular_values_order / init.singular_values_order[0];
                
                std::copy(init_i.begin(), init_i.end(),
                          &layer.weights[i][0]);
                layer.bias.fill(0.0);
                layer.ibias.fill(0.0);
                layer.iscales.fill(1.0);
                layer.hscales.fill(1.0);
                //layer.hscales = init.singular_values_order;
            }
        }

        //layer.zero_fill();
        //layer.bias.fill(0.01);
        //layer.weights[0][0] = -0.3;

        // Calculate the inputs to the next layer
        
        if (verbosity >= 3)
            cerr << "calculating next layer training inputs on "
                 << nx << " examples" << endl;
        double train_error_exact = 0.0, train_error_noisy = 0.0;
        boost::tie(train_error_exact, train_error_noisy)
            = layer.test_and_update(layer_train, next_layer_train,
                                    prob_cleared, thread_context,
                                    verbosity);

        if (verbosity >= 2)
            cerr << "training rmse of layer: exact "
                 << train_error_exact << " noisy " << train_error_noisy
                 << endl;
        
        if (verbosity >= 3)
            cerr << "calculating next layer testing inputs on "
                 << nxt << " examples" << endl;
        double test_error_exact = 0.0, test_error_noisy = 0.0;
        boost::tie(test_error_exact, test_error_noisy)
            = layer.test_and_update(layer_test, next_layer_test,
                                    prob_cleared, thread_context,
                                    verbosity);

#if 0
        push_back(layer);

        // Test the layer stack
        if (verbosity >= 3)
            cerr << "calculating whole stack testing performance on "
                 << nxt << " examples" << endl;
        boost::tie(test_error_exact, test_error_noisy)
            = test(testing_data, prob_cleared, thread_context, verbosity);
        
        if (verbosity >= 2)
            cerr << "testing rmse of stack: exact "
                 << test_error_exact << " noisy " << test_error_noisy
                 << endl;
        
        if (verbosity >= 2)
            cerr << "testing rmse of layer: exact "
                 << test_error_exact << " noisy " << test_error_noisy
                 << endl;

        pop_back();
#endif

        if (verbosity == 2)
            cerr << "iter  ---- train ----  ---- test -----\n"
                 << "        exact   noisy    exact   noisy\n";

        for (unsigned iter = 0;  iter < niter;  ++iter) {
            if (verbosity >= 3)
                cerr << "iter " << iter << " training on " << nx << " examples"
                     << endl;
            else if (verbosity >= 2)
                cerr << format("%4d", iter) << flush;
            Timer timer;

#if 0
            cerr << "weights: " << endl;
            for (unsigned i = 0;  i < 10;  ++i) {
                for (unsigned j = 0;  j < 10;  ++j) {
                    cerr << format("%7.4f", layer.weights[i][j]);
                }
                cerr << endl;
            }
            
            double max_abs_weight = 0.0;
            double total_abs_weight = 0.0;
            double total_weight_sqr = 0.0;
            for (unsigned i = 0;  i < ni;  ++i) {
                for (unsigned j = 0;  j < nh;  ++j) {
                    double abs_weight = abs(layer.weights[i][j]);
                    max_abs_weight = std::max(max_abs_weight, abs_weight);
                    total_abs_weight += abs_weight;
                    total_weight_sqr += abs_weight * abs_weight;
                }
            }

            double avg_abs_weight = total_abs_weight / (ni * nh);
            double rms_avg_weight = sqrt(total_weight_sqr / (ni * nh));

            cerr << "max = " << max_abs_weight << " avg = "
                 << avg_abs_weight << " rms avg = " << rms_avg_weight
                 << endl;
#endif

            //cerr << "iscales: " << layer.iscales << endl;
            //cerr << "hscales: " << layer.hscales << endl;
            //cerr << "bias: " << layer.bias << endl;
            //cerr << "ibias: " << layer.ibias << endl;

            distribution<LFloat> svalues(min(ni, nh));
            boost::multi_array<LFloat, 2> layer2 = layer.weights;
            int nvalues = std::min(ni, nh);
        
            boost::multi_array<LFloat, 2> rvectors(boost::extents[ni][nvalues]);
            boost::multi_array<LFloat, 2> lvectorsT(boost::extents[nvalues][nh]);

            int result = LAPack::gesdd("S", nh, ni,
                                       layer2.data(), nh,
                                       &svalues[0],
                                       &lvectorsT[0][0], nh,
                                       &rvectors[0][0], nvalues);
            if (result != 0)
                throw Exception("gesdd returned non-zero");
        

            if (false) {
                boost::multi_array<LFloat, 2> weights2
                    = rvectors * diag(svalues) * lvectorsT;
                
                cerr << "weights2: " << endl;
                for (unsigned i = 0;  i < 10;  ++i) {
                    for (unsigned j = 0;  j < 10;  ++j) {
                        cerr << format("%7.4f", weights2[i][j]);
                    }
                    cerr << endl;
                }
            }

            //if (iter == 0) layer.weights = rvectors * lvectorsT;

            //if (iter == 0) layer.weights = rvectors * lvectorsT;

            //cerr << "svalues = " << svalues << endl;

            double train_error_exact, train_error_noisy;
            boost::tie(train_error_exact, train_error_noisy)
                = layer.train_iter(layer_train, prob_cleared, thread_context,
                                   minibatch_size, learning_rate,
                                   verbosity, sample_proportion,
                                   randomize_order);

            if (verbosity >= 3) {
                cerr << "rmse of iteration: exact " << train_error_exact
                     << " noisy " << train_error_noisy << endl;
                if (verbosity >= 3) cerr << timer.elapsed() << endl;
            }
            else if (verbosity == 2)
                cerr << format("  %7.5f %7.5f",
                               train_error_exact, train_error_noisy)
                     << flush;

            if (iter % test_every == (test_every - 1)
                || iter == niter - 1) {
                timer.restart();
                double test_error_exact = 0.0, test_error_noisy = 0.0;
                
                if (verbosity >= 3)
                    cerr << "testing on " << nxt << " examples"
                         << endl;
                boost::tie(test_error_exact, test_error_noisy)
                    = layer.test(layer_test, prob_cleared, thread_context,
                                 verbosity);
                
                if (verbosity >= 3) {
                    cerr << "testing rmse of iteration: exact "
                         << test_error_exact << " noisy " << test_error_noisy
                         << endl;
                    cerr << timer.elapsed() << endl;
                }
                else if (verbosity == 2)
                    cerr << format("  %7.5f %7.5f",
                                   test_error_exact, test_error_noisy);
            }

            if (verbosity == 2) cerr << endl;
        }

        next_layer_train.resize(nx);
        next_layer_test.resize(nxt);

        // Calculate the inputs to the next layer
        
        if (verbosity >= 3)
            cerr << "calculating next layer training inputs on "
                 << nx << " examples" << endl;
        //double train_error_exact = 0.0, train_error_noisy = 0.0;
        boost::tie(train_error_exact, train_error_noisy)
            = layer.test_and_update(layer_train, next_layer_train,
                                    prob_cleared, thread_context,
                                    verbosity);

        if (verbosity >= 2)
            cerr << "training rmse of layer: exact "
                 << train_error_exact << " noisy " << train_error_noisy
                 << endl;
        
        if (verbosity >= 3)
            cerr << "calculating next layer testing inputs on "
                 << nxt << " examples" << endl;
        //double test_error_exact = 0.0, test_error_noisy = 0.0;
        boost::tie(test_error_exact, test_error_noisy)
            = layer.test_and_update(layer_test, next_layer_test,
                                    prob_cleared, thread_context,
                                    verbosity);
        
        if (verbosity >= 2)
            cerr << "testing rmse of layer: exact "
                 << test_error_exact << " noisy " << test_error_noisy
                 << endl;

        layer_train.swap(next_layer_train);
        layer_test.swap(next_layer_test);

        push_back(layer);

        // Test the layer stack
        if (verbosity >= 3)
            cerr << "calculating whole stack testing performance on "
                 << nxt << " examples" << endl;
        boost::tie(test_error_exact, test_error_noisy)
            = test_dnae(testing_data, prob_cleared, thread_context, verbosity);
        
        if (verbosity >= 2)
            cerr << "testing rmse of stack: exact "
                 << test_error_exact << " noisy " << test_error_noisy
                 << endl;
    }
}



} // namespace ML