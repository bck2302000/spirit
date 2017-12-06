#include <Spirit_Defines.h>
#include <engine/Method_MMF.hpp>
#include <engine/Vectormath.hpp>
#include <engine/Manifoldmath.hpp>
#include <io/IO.hpp>
#include <utility/Logging.hpp>

#include <Eigen/Core>
#include <Eigen/Eigenvalues>
//#include <unsupported/Eigen/CXX11/Tensor>
#include <GenEigsSolver.h>  // Also includes <MatOp/DenseGenMatProd.h>
#include <GenEigsRealShiftSolver.h>

#include <fmt/format.h>

using Utility::Log_Level;
using Utility::Log_Sender;

namespace Engine
{
    template <Solver solver>
    Method_MMF<solver>::Method_MMF(std::shared_ptr<Data::Spin_System_Chain_Collection> collection, int idx_chain) :
        Method_Solver<solver>(collection->parameters, -1, idx_chain), collection(collection)
    {
        int noc = collection->noc;
        this->nos = collection->chains[0]->images[0]->nos;
        switched1 = false;
        switched2 = false;
        this->SenderName = Utility::Log_Sender::MMF;
        
        // The systems we use are the last image of each respective chain
        for (int ichain = 0; ichain < noc; ++ichain)
        {
            this->systems.push_back(this->collection->chains[ichain]->images.back());
        }
        this->noi = this->systems.size();

        // History
        this->history = std::map<std::string, std::vector<scalar>>{
            {"max_torque_component", {this->force_max_abs_component}} };

        // We assume that the systems are not converged before the first iteration
        this->force_max_abs_component = this->collection->parameters->force_convergence + 1.0;

        this->hessian = std::vector<MatrixX>(noc, MatrixX(3*this->nos, 3*this->nos));	// [noc][3nos]
        // Forces
        this->gradient   = std::vector<vectorfield>(noc, vectorfield(this->nos));	// [noc][3nos]
        this->minimum_mode = std::vector<vectorfield>(noc, vectorfield(this->nos));	// [noc][3nos]
        this->xi = vectorfield(this->nos, {0,0,0});

        // Last iteration
        this->spins_last = std::vector<vectorfield>(noc, vectorfield(this->nos));	// [noc][3nos]
        this->spins_last[0] = *this->systems[0]->spins;
        this->Rx_last = 0.0;

        // Force function
        // ToDo: move into parameters
        this->mm_function = "Spectra Matrix"; // "Spectra Matrix" "Spectra Prefactor" "Lanczos"


        // Create shared pointers to the method's systems' spin configurations
        this->configurations = std::vector<std::shared_ptr<vectorfield>>(this->noi);
        for (int i = 0; i<this->noi; ++i) this->configurations[i] = this->systems[i]->spins;
    }


    template <Solver solver>
    void Method_MMF<solver>::Calculate_Force(const std::vector<std::shared_ptr<vectorfield>> & configurations, std::vector<vectorfield> & forces)
    {
        if (this->mm_function == "Spectra Matrix")
        {
            this->Calculate_Force_Spectra_Matrix(configurations, forces);
        }
        else if (this->mm_function == "Lanczos")
        {
            this->Calculate_Force_Lanczos(configurations, forces);
        }
        #ifdef SPIRIT_ENABLE_PINNING
            Vectormath::set_c_a(1, forces[0], forces[0], this->parameters->pinning->mask_unpinned);
        #endif // SPIRIT_ENABLE_PINNING
    }

    template <Solver solver>
    void Method_MMF<solver>::Calculate_Force_Spectra_Matrix(const std::vector<std::shared_ptr<vectorfield>> & configurations, std::vector<vectorfield> & forces)
    {
        const int nos = configurations[0]->size();
        
        // Number of lowest modes to be calculated
        // NOTE THE ORDER OF THE MODES: the first eigenvalue is not necessarily the lowest for n>1
        int n_modes = 1;
        
        // Loop over chains and calculate the forces
        for (int ichain = 0; ichain < this->collection->noc; ++ichain)
        {
            auto& image = *configurations[ichain];
            auto& grad = gradient[ichain];
            auto& hess = hessian[ichain];

            // The gradient (unprojected)
            this->systems[ichain]->hamiltonian->Gradient(image, grad);
            Vectormath::set_c_a(1, grad, grad, this->parameters->pinning->mask_unpinned);

            // The Hessian (unprojected)
            this->systems[ichain]->hamiltonian->Hessian(image, hess);

            // Calculate the final Hessian to use for the minimum mode
            MatrixX hessian_final = MatrixX::Zero(2*nos, 2*nos);
            // Manifoldmath::hessian_bordered(image, grad, hess, hessian_final);
            // Manifoldmath::hessian_projected(image, grad, hess, hessian_final);
            Manifoldmath::hessian_weingarten(image, grad, hess, hessian_final);
            // Manifoldmath::hessian_spherical(image, grad, hess, hessian_final);
            // Manifoldmath::hessian_covariant(image, grad, hess, hessian_final);


            // Create a Spectra solver
            Spectra::DenseGenMatProd<scalar> op(hessian_final);
            Spectra::GenEigsSolver< scalar, Spectra::SMALLEST_REAL, Spectra::DenseGenMatProd<scalar> > hessian_spectrum(&op, n_modes, 2*nos);
            // Spectra::GenEigsSolver< scalar, Spectra::SMALLEST_REAL, Spectra::DenseGenMatProd<scalar> > hessian_spectrum_reference(&op, 1, 2*nos);
            hessian_spectrum.init();
            // hessian_spectrum_reference.init();
            // Compute the specified spectrum
            int nconv = hessian_spectrum.compute();
            // int xxxxx = hessian_spectrum_reference.compute();

            // If eigenvalue calculation was successful, we continue
            if (hessian_spectrum.info() == Spectra::SUCCESSFUL)
            {
                // Retrieve the Eigenvalues
                VectorX evalues = hessian_spectrum.eigenvalues().real();
                // scalar eval_lowest = hessian_spectrum_reference.eigenvalues().real()[0];
                scalar eval_lowest = evalues[0];

                // Retrieve the Eigenvectors
                MatrixX evectors = hessian_spectrum.eigenvectors().real();
                Eigen::Ref<VectorX> evec_lowest_2N = evectors.col(0);

                // Extract the minimum mode (transform evec_lowest_2N back to 3N)
                MatrixX basis_3Nx2N = MatrixX::Zero(3*nos, 2*nos);
                Manifoldmath::tangent_basis_spherical(image, basis_3Nx2N); // Important to choose the right matrix here! It should especially be consistent with the matrix chosen in the Hessian calculation!
                VectorX evec_lowest_3N = basis_3Nx2N * evec_lowest_2N;
                for (int n=0; n<nos; ++n)
                    this->minimum_mode[ichain][n] = {evec_lowest_3N[3*n], evec_lowest_3N[3*n+1], evec_lowest_3N[3*n+2]};


                ////////////////////////////////////////////////////////////////
                // Check for complex numbers
                if (std::abs(hessian_spectrum.eigenvalues().imag()[0]) > 1e-8)
                    std::cerr << "     >>>>>>>> WARNING  nonzero complex EW    WARNING" << std::endl; 
                for (int ispin=0; ispin<nos; ++ispin)
                {
                    if (std::abs(hessian_spectrum.eigenvectors().col(0).imag()[0]) > 1e-8)
                        std::cerr << "     >>>>>>>> WARNING  nonzero complex EV x  WARNING" << std::endl; 
                    if (std::abs(hessian_spectrum.eigenvectors().col(0).imag()[1]) > 1e-8)
                        std::cerr << "     >>>>>>>> WARNING  nonzero complex EV y  WARNING" << std::endl; 
                    if (std::abs(hessian_spectrum.eigenvectors().col(0).imag()[2]) > 1e-8)
                        std::cerr << "     >>>>>>>> WARNING  nonzero complex EV z  WARNING" << std::endl; 
                }
                ////////////////////////////////////////////////////////////////


                ////////////////////////////////////////////////////////////////
                // For one of the tests
                auto grad_tangential = grad;
                Manifoldmath::project_tangential(grad_tangential, image);
                // Get the tangential gradient in 2N-representation
                Eigen::Ref<VectorX> grad_tangent_3N = Eigen::Map<VectorX>(grad_tangential[0].data(), 3 * nos);
                VectorX grad_tangent_2N             = basis_3Nx2N.transpose() * grad_tangent_3N;
                /////////
                // Norms
                scalar image_norm           = Manifoldmath::norm(image);
                scalar grad_norm            = Manifoldmath::norm(grad);
                scalar grad_tangent_norm    = Manifoldmath::norm(grad_tangential);
                scalar mode_norm            = Manifoldmath::norm(minimum_mode[ichain]);
                scalar mode_norm_2N         = evec_lowest_2N.norm();
                // Scalar products
                scalar mode_dot_image       = std::abs(Vectormath::dot(minimum_mode[ichain], image) / mode_norm); // mode should be orthogonal to image in 3N-space
                scalar mode_grad_angle      = std::abs(evec_lowest_3N.dot(grad_tangent_3N) / evec_lowest_3N.norm() / grad_tangent_3N.norm());
                scalar mode_grad_angle_2N   = std::abs(evec_lowest_2N.dot(grad_tangent_2N) / evec_lowest_2N.norm() / grad_tangent_2N.norm());
                // Do some more checks to ensure the mode fulfills our requirements
                bool bad_image_norm         = 1e-8  < std::abs( image_norm - std::sqrt((scalar)nos) ); // image norm should be sqrt(nos)
                bool bad_grad_norm          = 1e-8  > grad_norm;         // gradient should not be a zero vector
                bool bad_grad_tangent_norm  = 1e-8  > grad_tangent_norm; // gradient should not be a zero vector in tangent space
                bool bad_mode_norm          = 1e-8  > mode_norm;         // mode should not be a zero vector
                /////////
                bool bad_mode_dot_image     = 1e-10 < mode_dot_image;     // mode should be orthogonal to image in 3N-space
                bool bad_mode_grad_angle    = 1e-8  > mode_grad_angle;    // mode should not be orthogonal to gradient in 3N-space
                bool bad_mode_grad_angle_2N = 1e-8  > mode_grad_angle_2N; // mode should not be orthogonal to gradient in 2N-space
                /////////
                bool eval_nonzero           = 1e-8  < std::abs(eval_lowest);
                /////////
                if ( bad_image_norm     || bad_mode_norm       || bad_grad_norm          || bad_grad_tangent_norm ||
                     bad_mode_dot_image || ( eval_nonzero && (bad_mode_grad_angle || bad_mode_grad_angle_2N) ) )
                {
                    // scalar theta, phi;
                    // Manifoldmath::spherical_from_cartesian(image[1], theta, phi);
                    std::cerr << "-------------------------"                       << std::endl;
                    std::cerr << "BAD MODE! evalue =      " << evalues[0]          << std::endl;
                    // std::cerr << "image (theta,phi):      " << theta << " " << phi << std::endl;
                    std::cerr << "image norm:             " << image_norm          << std::endl;
                    std::cerr << "mode norm:              " << mode_norm           << std::endl;
                    std::cerr << "mode norm 2N:           " << mode_norm_2N        << std::endl;
                    std::cerr << "grad norm:              " << grad_norm           << std::endl;
                    std::cerr << "grad norm tangential:   " << grad_tangent_norm   << std::endl;
                    if (bad_image_norm)
                        std::cerr << "   image norm is not equal to sqrt(nos): " << image_norm << std::endl;
                    if (bad_mode_norm)
                        std::cerr << "   mode norm is too small: "               << mode_norm  << std::endl;
                    if (bad_grad_norm)
                        std::cerr << "   gradient norm is too small: "           << grad_norm  << std::endl;
                    if (bad_mode_dot_image)
                    {
                        std::cerr << "   mode NOT TANGENTIAL to SPINS: "         << mode_dot_image << std::endl;
                        std::cerr << "             >>> check the (3N x 2N) spherical basis matrix" << std::endl;
                    }
                    if ( eval_nonzero && (bad_mode_grad_angle || bad_mode_grad_angle_2N) )
                    {
                        std::cerr << "   mode is ORTHOGONAL to GRADIENT: 3N = " << mode_grad_angle << std::endl;
                        std::cerr << "                              >>>  2N = " << mode_grad_angle_2N << std::endl;
                    }
                    std::cerr << "-------------------------" << std::endl;
                }
                ////////////////////////////////////////////////////////////////



                // Make sure the mode is a tangent vector
                Manifoldmath::project_tangential(this->minimum_mode[ichain], image);
                // Normalize the mode vector in 3N dimensions
                Manifoldmath::normalize(this->minimum_mode[ichain]);
                
                // std::cerr << "eval ref: " << hessian_spectrum_reference.eigenvalues().transpose() << std::endl;
                // Check if the lowest eigenvalue is negative
                if (eval_lowest <= 0 && mode_grad_angle > 1e-8)// -1e-6)// || switched2)
                {
                    // Scalar product of mode and gradient
                    scalar lambda_F = Vectormath::dot(this->minimum_mode[ichain], grad);

                    std::cerr << fmt::format("negative region: {:<20}   angle = {:15.10f}   lambda*F = {:15.10f}", evalues.transpose(), std::acos(std::min(mode_grad_angle,1.0))*180.0/M_PI, std::abs(lambda_F)) << std::endl;

                    // Invert the gradient force along the minimum mode
                    Manifoldmath::invert_parallel(grad, minimum_mode[ichain]);

                    // Copy out the forces
                    Vectormath::set_c_a(-1, grad, forces[ichain], collection->parameters->pinning->mask_unpinned);
                }
                // Otherwise we seek for the lowest nonzero eigenvalue
                else if (mode_grad_angle > 1e-8)
                {
                    // Scalar product of mode and gradient
                    scalar lambda_F = Vectormath::dot(this->minimum_mode[ichain], grad);

                    std::cerr << fmt::format("positive region: {:<20}   angle = {:15.10f}   lambda*F = {:15.10f}", evalues.transpose(), std::acos(std::min(mode_grad_angle,1.0))*180.0/M_PI, std::abs(lambda_F)) << std::endl;

                    // Calculate the force
                    Vectormath::set_c_a(lambda_F, this->minimum_mode[ichain], forces[ichain], collection->parameters->pinning->mask_unpinned);

                    // // Copy out the forces
                    // Vectormath::set_c_a(1, grad, forces[ichain], collection->parameters->pinning->mask_unpinned);
                }
                else
                {
                    // Scalar product of mode and gradient
                    scalar lambda_F = Vectormath::dot(this->minimum_mode[ichain], grad);

                    if (std::abs(eval_lowest) > 1e-8)
                        std::cerr << fmt::format("bad region:        {:<20}   angle = {:15.10f}   lambda*F = {:15.10f}", evalues.transpose(), std::acos(std::min(mode_grad_angle,1.0))*180.0/M_PI, std::abs(lambda_F)) << std::endl;
                    else
                        std::cerr << fmt::format("zero region:       {:<20}   angle = {:15.10f}   lambda*F = {:15.10f}", evalues.transpose(), std::acos(std::min(mode_grad_angle,1.0))*180.0/M_PI, std::abs(lambda_F)) << std::endl;

                        // Copy out the forces
                    Vectormath::set_c_a(1, grad, forces[ichain], collection->parameters->pinning->mask_unpinned);
                }
            }
            else
            {
                // Spectra was not successful in calculating an eigenvector
                Log(Log_Level::Error, Log_Sender::MMF, "Failed to calculate eigenvectors of the Hessian!");
                Log(Log_Level::Info, Log_Sender::MMF, "Zeroing the MMF force...");
                for (auto& x : forces[ichain]) x.setZero();
            }
        }
    }




    // This Lanczos algorithm is implemented as described in this paper:
    // R. A. Olsen, G. J. Kroes, G. Henkelman, A. Arnaldsson, and H. Jónsson, 
    // Comparison of methods for finding saddle points without knowledge of the final states,
    // J. Chem. Phys. 121, 9776-9792 (2004).
    //
    // The 1 character variables in this method match the variables in the
    // equations in the paper given above
    template <Solver solver>
    void Method_MMF<solver>::Calculate_Force_Lanczos(const std::vector<std::shared_ptr<vectorfield>> configurations, std::vector<vectorfield> & forces)
    {
        scalar lowestEw;
        MatrixX lowestEv;

        const int nos = configurations[0]->size();
        const int size = 3*nos;
        auto& image = *configurations[0];
        
        int lanczosMaxIterations = 100;
        scalar lanczosTolerance = 1e-5;
        scalar finiteDifference = 1e-3;

        MatrixX T(size, lanczosMaxIterations), Q(size, lanczosMaxIterations);
        T.setZero();
        VectorX u(size);//, r(size);

        // Convert the AtomMatrix of all the atoms into
        // a single column vector with just the free coordinates.
        int i,j;
        Eigen::Ref<VectorX> r = Eigen::Map<VectorX>(image[0].data(), 3 * nos);
        // for (i=0,j=0;i<nos;i++)
        // {
        // 	// if (!matter->getFixed(i)) // pinning and vacancies need to be considered here
        // 	// {
        // 		r.segment<3>(j) = direction.row(i);
        // 		j+=3;
        // 	// }
        // }

        scalar alpha, beta=r.norm();
        scalar ew=0, ewOld=0, ewAbsRelErr;
        scalar dr = finiteDifference;
        VectorX evEst, evT, evOldEst;

        vectorfield spins_tmp = image, force_tmp(nos);
        // Matter *tmpMatter = new Matter(parameters);
        // *tmpMatter = *matter;

        this->systems[0]->hamiltonian->Gradient(spins_tmp, force_tmp);
        Eigen::Ref<VectorX> force1 = Eigen::Map<VectorX>(force_tmp[0].data(), size);
        // force1 = tmpMatter->getForcesFreeV();

        for (i=0; i<size; ++i)
        {
            Q.col(i) = r/beta;

            // Finite difference force in the direction of the ith Lanczos vector
            for (int _i=0; _i < nos; ++i)
                spins_tmp[i] += dr*Q.col(i).segment<3>(_i);
            this->systems[0]->hamiltonian->Gradient(spins_tmp, force_tmp);
            Eigen::Ref<VectorX> force2 = Eigen::Map<VectorX>(force_tmp[0].data(), size);
            // tmpMatter->setPositionsFreeV(matter->getPositionsFreeV()+dr*Q.col(i));
            // force2 = tmpMatter->getForcesFreeV();

            u = -(force2-force1)/dr;

            if (i==0)
                r = u;
            else
                r = u-beta*Q.col(i-1);
            
            alpha = Q.col(i).dot(r);
            r = r-alpha*Q.col(i);

            T(i,i) = alpha;
            if (i>0)
            {
                T(i-1,i) = beta;
                T(i,i-1) = beta;
            }

            beta = r.norm();

            if (beta <= 1e-10*fabs(alpha))
            {
                /* If Q(0) is an eigenvector (or a linear combination of a subset of eigenvectors)
                then the lanczos cannot complete the basis of vector Q.*/
                if (i == 0)
                {
                    ew = alpha;
                    evEst = Q.col(0);
                }
                // log_file("[ILanczos] ERROR: linear dependence\n");
                std::cerr << "[ILanczos] ERROR: linear dependence" << std::endl;
                break;
            }
            //Check Eigenvalues
            if (i >= 1)
            {
                Eigen::SelfAdjointEigenSolver<MatrixX> es(T.block(0,0,i+1,i+1));
                ew = es.eigenvalues()(0); 
                evT = es.eigenvectors().col(0);
                ewAbsRelErr = fabs((ew-ewOld)/ewOld);
                ewOld = ew;

                //Convert eigenvector of T matrix to eigenvector of full Hessian
                evEst = Q.block(0,0,size,i+1)*evT;
                evEst.normalize();
                // statsAngle = acos(fabs(evEst.dot(evOldEst)))*(180/M_PI);
                // statsTorque = ewAbsRelErr;
                evOldEst = evEst;
                // log_file("[ILanczos] %9s %9s %10s %14s %9.4f %10.6f %7.3f %5i\n", 
                // "----", "----", "----", "----", ew, ewAbsRelErr, statsAngle, i);
                if (ewAbsRelErr < lanczosTolerance)
                {
                    // log_file("[ILanczos] Tolerence reached: %f\n", lanczosTolerance);
                    std::cerr << "[ILanczos] Tolerence reached: " << ewAbsRelErr << std::endl;
                    break;
                }
            }
            else
            {
                ew = alpha;
                ewOld = ew;
                evEst = Q.col(0);
                evOldEst = Q.col(0);
                if (lowestEw != 0.0 && false) // && parameters->lanczosQuitEarly)
                {
                    double Cprev = lowestEw;
                    double Cnew = u.dot(Q.col(i));
                    ewAbsRelErr = fabs((Cnew-Cprev)/Cprev);
                    if (ewAbsRelErr <= lanczosTolerance)
                    {
                        // statsAngle = 0.0;
                        // statsTorque = ewAbsRelErr;
                        // log_file("[ILanczos] Tolerence reached: %f\n", lanczosTolerance);
                        std::cerr << "[ILanczos] Tolerence reached: " << ewAbsRelErr << std::endl;
                        break;
                    }
                }
            }

            if (i >= lanczosMaxIterations-1)
            {
                // log_file("[ILanczos] Max iterations\n");
                std::cerr << "[ILanczos] Max iterations" << std::endl;
                break;
            }
        }

        lowestEw = ew;

        // Convert back from free atom coordinate column vector
        // to AtomMatrix style.
        lowestEv.resize(nos,3);
        for (i=0,j=0;i<nos;i++)
        {
            // if (!matter->getFixed(i))
            // {
                lowestEv.row(i) = evEst.segment<3>(j);
                j+=3;
            // }
        }
    }



    void printmatrix(MatrixX & m)
    {
        std::cerr << m << std::endl;
    }

        
    // Check if the Forces are converged
    template <Solver solver>
    bool Method_MMF<solver>::Converged()
    {
        if (this->force_max_abs_component < this->collection->parameters->force_convergence) return true;
        return false;
    }

    template <Solver solver>
    void Method_MMF<solver>::Hook_Pre_Iteration()
    {

    }

    template <Solver solver>
    void Method_MMF<solver>::Hook_Post_Iteration()
    {
        // --- Convergence Parameter Update
        this->force_max_abs_component = 0;
        for (int ichain = 0; ichain < collection->noc; ++ichain)
        {
            scalar fmax = this->Force_on_Image_MaxAbsComponent(*(this->systems[ichain]->spins), gradient[ichain]);
            if (fmax > this->force_max_abs_component) this->force_max_abs_component = fmax;
        }

        // --- Update the chains' last images
        for (auto system : this->systems) system->UpdateEnergy();
        for (auto chain : collection->chains)
        {
            int i = chain->noi - 1;
            if (i>0) chain->Rx[i] = chain->Rx[i - 1] + Manifoldmath::dist_geodesic(*chain->images[i]->spins, *chain->images[i-1]->spins);
        }
    }

    template <Solver solver>
    void Method_MMF<solver>::Save_Current(std::string starttime, int iteration, bool initial, bool final)
    {
        // History save
        this->history["max_torque_component"].push_back(this->force_max_abs_component);

        // File save
        if (this->parameters->output_any)
        {
            // Convert indices to formatted strings
            auto s_img  = fmt::format("{:0>2}", this->idx_image);
            int base = (int)log10(this->parameters->n_iterations);
            std::string s_iter = fmt::format("{:0>"+fmt::format("{}",base)+"}", iteration);

            std::string preSpinsFile;
            std::string preEnergyFile;
            std::string fileTag;
            
            if (this->systems[0]->llg_parameters->output_file_tag == "<time>")
                fileTag = starttime + "_";
            else if (this->systems[0]->llg_parameters->output_file_tag != "")
                fileTag = this->systems[0]->llg_parameters->output_file_tag + "_";
            else
                fileTag = "";
                
            preSpinsFile = this->parameters->output_folder + "/" + fileTag + "Image-" + s_img + "_Spins";
            preEnergyFile = this->parameters->output_folder + "/"+ fileTag + "Image-" + s_img + "_Energy";

            // Function to write or append image and energy files
            auto writeOutputConfiguration = [this, preSpinsFile, preEnergyFile, iteration](std::string suffix, bool append)
            {
                // File name and comment
                std::string spinsFile = preSpinsFile + suffix + ".txt";
                std::string comment = std::to_string( iteration );
                // Spin Configuration
                IO::Write_Spin_Configuration( *( this->systems[0] )->spins, 
                                              *( this->systems[0] )->geometry, spinsFile, 
                                              IO::VF_FileFormat::SPIRIT_WHITESPACE_SPIN, 
                                              comment, append );
            };

            auto writeOutputEnergy = [this, preSpinsFile, preEnergyFile, iteration](std::string suffix, bool append)
            {
                bool normalize = this->systems[0]->llg_parameters->output_energy_divide_by_nspins;

                // File name
                std::string energyFile = preEnergyFile + suffix + ".txt";
                std::string energyFilePerSpin = preEnergyFile + "-perSpin" + suffix + ".txt";

                // Energy
                if (append)
                {
                    // Check if Energy File exists and write Header if it doesn't
                    std::ifstream f(energyFile);
                    if (!f.good()) IO::Write_Energy_Header(*this->systems[0], energyFile);
                    // Append Energy to File
                    IO::Append_Image_Energy(*this->systems[0], iteration, energyFile, normalize);
                }
                else
                {
                    IO::Write_Energy_Header(*this->systems[0], energyFile);
                    IO::Append_Image_Energy(*this->systems[0], iteration, energyFile, normalize);
                    if (this->systems[0]->llg_parameters->output_energy_spin_resolved)
                    {
                        IO::Write_Image_Energy_per_Spin(*this->systems[0], energyFilePerSpin, normalize);
                    }
                }
            };


            // Initial image before simulation
            if (initial && this->parameters->output_initial)
            {
                writeOutputConfiguration("-initial", false);
                writeOutputEnergy("-initial", false);
            }
            // Final image after simulation
            else if (final && this->parameters->output_final)
            {
                writeOutputConfiguration("-final", false);
                writeOutputEnergy("-final", false);
            }
            
            // Single file output
            if (this->systems[0]->llg_parameters->output_configuration_step)
            {
                writeOutputConfiguration("_" + s_iter, false);
            }
            if (this->systems[0]->llg_parameters->output_energy_step)
            {
                writeOutputEnergy("_" + s_iter, false);
            }

            // Archive file output (appending)
            if (this->systems[0]->llg_parameters->output_configuration_archive)
            {
                writeOutputConfiguration("-archive", true);
            }
            if (this->systems[0]->llg_parameters->output_energy_archive)
            {
                writeOutputEnergy("-archive", true);
            }
        }
    }

    template <Solver solver>
    void Method_MMF<solver>::Finalize()
    {
        this->collection->iteration_allowed=false;
    }

    template <Solver solver>
    bool Method_MMF<solver>::Iterations_Allowed()
    {
        return this->collection->iteration_allowed;
    }

    // Method name as string
    template <Solver solver>
    std::string Method_MMF<solver>::Name() { return "MMF"; }

    // Template instantiations
    template class Method_MMF<Solver::SIB>;
    template class Method_MMF<Solver::Heun>;
    template class Method_MMF<Solver::Depondt>;
    template class Method_MMF<Solver::NCG>;
    template class Method_MMF<Solver::VP>;
}