#include "../../include/interactive_mid_protocols/OTExtensionMalicious.hpp"

const char* OTExtensionMaliciousBase::m_initial_seed = "437398417012387813714564100";

OTExtensionMaliciousBase::OTExtensionMaliciousBase(int role, int num_base_ots, int num_ots) {
	m_num_base_ots = num_base_ots;
	m_num_ots = num_ots;

	// set to a fixed default
	m_counter = 0;
	m_security_level = maliciousot::LT;
	m_num_checks = 380;

	// init seeds
	init_seeds(role);

	// init base ot handler
	m_baseot_handler = new maliciousot::PVWDDH(m_security_level, m_receiver_seed);
}
OTExtensionMaliciousBase::~OTExtensionMaliciousBase() {
	delete m_baseot_handler;
}

void OTExtensionMaliciousBase::init_seeds(int role) {
	BYTE seedtmp[SHA1_BYTES];
	maliciousot::HASH_CTX sha;

	// m_receiver_seed = hash(role || m_initial_seed)
	MPC_HASH_INIT(&sha);
	MPC_HASH_UPDATE(&sha, (BYTE*)&role, sizeof(role));
	MPC_HASH_UPDATE(&sha, (BYTE*)m_initial_seed, sizeof(m_initial_seed));
	MPC_HASH_FINAL(&sha, m_receiver_seed);

	// m_sender_seed = hash(role || m_receiver_seed)
	MPC_HASH_INIT(&sha);
	MPC_HASH_UPDATE(&sha, (BYTE*)&role, sizeof(role));
	MPC_HASH_UPDATE(&sha, (BYTE*)m_receiver_seed, SHA1_BYTES);
	MPC_HASH_FINAL(&sha, seedtmp);
	memcpy(m_sender_seed, seedtmp, AES_BYTES);
}

OTExtensionMaliciousSender::OTExtensionMaliciousSender(SocketPartyData bindAddress, int numOts, int numOfThreads, int numBaseOts) : OTExtensionMaliciousBase(1, numBaseOts, numOts) {
	m_connection_manager = new ConnectionManagerServer(0, numOfThreads, bindAddress);

	int nSndVals = 2;
	int wdsize = 1 << (maliciousot::CEIL_LOG2(m_num_base_ots));
	int nblocks = CEIL_DIVIDE(m_num_ots, NUMOTBLOCKS * wdsize);
	int s2ots = nblocks * m_num_base_ots;

	// key seed matrix used for the 1-step base OTs
	m_receiver_key_seeds_matrix = (BYTE*)malloc(AES_KEY_BYTES * m_num_base_ots * nSndVals);
	// key seeds for the 2-nd step base OTs
	m_sender_key_seeds = (BYTE*)malloc(AES_KEY_BYTES * s2ots);//m_security_level.symbits);

															  // Server listen
	
	//m_connection_manager = unique_ptr<ConnectionManager>(tempS);
	m_connection_manager->setup_connection();

	// 1st step: precompute base ot
	precompute_base_ots_sender();

	maliciousot::CBitVector seedcbitvec;
	maliciousot::CBitVector U(s2ots, m_receiver_seed, m_counter);
	maliciousot::CBitVector URev(s2ots);

	seedcbitvec.AttachBuf(m_sender_key_seeds, AES_KEY_BYTES * s2ots);

	maliciousot::XORMasking* masking_function = new maliciousot::XORMasking(AES_KEY_BITS);

	assert(nblocks <= NUMOTBLOCKS);

	// 2nd step: OT extension step to obtain the base-OTs for the next step
	m_receiver = new maliciousot::Mal_OTExtensionReceiver(nSndVals, m_security_level.symbits,
		m_connection_manager->get_sockets_data(),
		m_receiver_key_seeds_matrix,
		m_receiver_seed, m_num_base_ots, s2ots);
	//m_receiver = unique_ptr<Mal_OTExtensionReceiver>(tempR);
	m_receiver->receive(s2ots, AES_KEY_BITS, U, seedcbitvec, maliciousot::R_OT, 1, masking_function);
	delete masking_function;

	for (int i = 0; i < s2ots; i++) {
		URev.SetBit(i, U.GetBitNoMask(i));
	}

	m_sender = new maliciousot::Mal_OTExtensionSender(nSndVals, m_security_level.symbits,
		m_connection_manager->get_sockets_data(),
		URev, m_sender_key_seeds, m_num_base_ots,
		m_num_checks, s2ots, m_sender_seed);
	//m_sender = unique_ptr<Mal_OTExtensionSender>(tempSnd);
	
}

BOOL OTExtensionMaliciousSender::precompute_base_ots_sender() {
	int nSndVals = 2;
	// Execute NP receiver routine and obtain the key 
	BYTE* pBuf = new BYTE[SHA1_BYTES * m_num_base_ots * nSndVals];

	//=================================================	
	m_baseot_handler->Sender(nSndVals, m_num_base_ots, m_connection_manager->get_socket(0), pBuf);

	BYTE* pBufIdx = pBuf;
	for (int i = 0; i<m_num_base_ots * nSndVals; i++) {
		memcpy(m_receiver_key_seeds_matrix + i * AES_KEY_BYTES, pBufIdx, AES_KEY_BYTES);
		pBufIdx += SHA1_BYTES;
	}

	delete[] pBuf;

	return true;
}

/**
* The overloaded function that runs the protocol.<p>
* After the base OT was done by the constructor, call to this function will be optimized and fast, no matter how much OTs there are.
* @param channel Disregarded. This is ignored since the connection is done in the c++ code.
* @param input The input for the sender specifying the version of the OT extension to run.
* Every call to the transfer function can run a different OT extension version.
*/
shared_ptr<OTBatchSOutput> OTExtensionMaliciousSender::transfer(OTBatchSInput * input) {

	int numOfOts;

	// In case the given input is general input.
	if (input->getType() == OTBatchSInputTypes::OTExtensionGeneralSInput) {

		//Retrieve the values from the input object.
		auto x0 = ((OTExtensionGeneralSInput *)input)->getX0Arr();
		auto x1 = ((OTExtensionGeneralSInput *)input)->getX1Arr();
		numOfOts = ((OTExtensionGeneralSInput *)input)->getNumOfOts();
		auto x0Size = ((OTExtensionGeneralSInput *)input)->getX0ArrSize();

		//Call the native function.
		int bitLength = (x0Size / numOfOts) * 8;
		vector<byte> empty;
		runOtAsSender(x0, x1, empty, numOfOts, bitLength, maliciousot::G_OT);

		//This version has no output. Return null.
		return NULL;

		//In case the given input is correlated input.
		/*
		}
		else if (input instanceof OTExtensionCorrelatedSInput) {

		byte[] delta = ((OTExtensionCorrelatedSInput)input).getDelta();

		// Prepare empty x0 and x1 for the output.
		byte[] x0 = new byte[delta.length];
		byte[] x1 = new byte[delta.length];

		numOfOts = ((OTExtensionCorrelatedSInput)input).getNumOfOts();

		//Call the native function. It will fill x0 and x1.
		runOtAsSender(senderPtr, x0, x1, delta, numOfOts, delta.length / numOfOts * 8, OT_EXTENSION_TYPE_CORRELATED);

		//Return output contains x0, x1.
		return new OTExtensionSOutput(x0, x1);

		//In case the given input is random input.
		}
		else if (input instanceof OTExtensionRandomSInput) {

		numOfOts = ((OTExtensionRandomSInput)input).getNumOfOts();
		int bitLength = ((OTExtensionRandomSInput)input).getBitLength();

		//Prepare empty x0 and x1 for the output.
		byte[] x0 = new byte[numOfOts * bitLength / 8];
		byte[] x1 = new byte[numOfOts * bitLength / 8];

		//Call the native function. It will fill x0 and x1.
		runOtAsSender(senderPtr, x0, x1, null, numOfOts, bitLength, OT_EXTENSION_TYPE_RANDOM);

		//Return output contains x0, x1.
		return new OTExtensionSOutput(x0, x1);

		//If input is not instance of the above inputs, throw Exception.*/
	}
	else {
		throw invalid_argument("input should be an instance of OTExtensionGeneralSInput or OTExtensionCorrelatedSInput or OTExtensionRandomSInput.");
	}
}

/*
* runs the OT extension as the sender.
* @param x0 An array that holds all the x0 values for each of the OT's serially (concatenated).
* @param x1 An array that holds all the x1 values for each of the OT's serially (concatenated).
* @param delta
* @param numOfOts The number of OTs that the protocol runs (how many strings are inside x0?)
* @param bitLength The length (in bits) of each item in the OT. can be derived from |x0|, |x1|, numOfOts
* @param version the OT extension version the user wants to use.
*/
void OTExtensionMaliciousSender::runOtAsSender(vector<byte> x0, vector<byte> x1, vector<byte> delta, int numOfOts, int bitLength, BYTE version) {
	
	maliciousot::CBitVector /*delta,*/ X1, X2;
	maliciousot::MaskingFunction * masking_function = new maliciousot::XORMasking(bitLength);
	//Create X1 and X2 as two arrays with "numOTs" entries of "bitlength" bit-values
	X1.Create(numOfOts, bitLength);
	X2.Create(numOfOts, bitLength);


	// general ot ----------------------------------------------------------------

	if (version == maliciousot::G_OT) {
		//copy the values given from java
		for (int i = 0; i < numOfOts*bitLength / 8; i++)
		{
			X1.SetByte(i, x0.at(i));
			X2.SetByte(i, x1.at(i));
		}
	}

	/* correlated ot -------------------------------------------------------------
	else if (version == C_OT) {
		//get the delta from java
		deltaArr = env->GetByteArrayElements(deltaFromJava, 0);
		delta.Create(numOfOts, bitLength);

		// set the delta values given from java
		int deltaSizeInBytes = numOfOts * bitLength / 8;
		for (int i = 0; i < deltaSizeInBytes; i++) {
			delta.SetByte(i, deltaArr[i]);
		}

		//creates delta as an array with "numOTs" entries of "bitlength" 
		// bit-values and fills delta with random values
		//delta.Create(numOfOts, bitLength, m_aSeed, m_nCounter);
	}

	// random ot -----------------------------------------------------------------
	else if (version == R_OT) {
		//no need to set any values. There is no input for x0 and x1 and no input for delta
	}*/

	m_sender->send(numOfOts, bitLength, X1, X2, version, m_connection_manager->get_num_of_threads(), masking_function);

	/*if (version != G_OT) { //we need to copy x0 and x1 

					   //get the values from the ot and copy them to x1Arr, x2Arr wich later on will be copied to the java values x1 and x2
		for (int i = 0; i < numOfOts*bitLength / 8; i++) {
			//copy each byte result to out
			x1Arr[i] = X1.GetByte(i);
			x2Arr[i] = X2.GetByte(i);
		}

		if (ver == C_OT) {
			env->ReleaseByteArrayElements(deltaFromJava, deltaArr, 0);
		}
	}*/
	delete masking_function;

	X1.delCBitVector();
	X2.delCBitVector();
	//delta.delCBitVector();
}

/**
* A constructor that creates the native receiver with communication abilities. <p>
* It uses the ip address and port given in the party object.<p>
* The construction runs the base OT phase. Further calls to transfer function will be optimized and fast, no matter how much OTs there are.
* @param party An object that holds the ip address and port.
* @param koblitzOrZpSize An integer that determines whether the OT extension uses Zp or ECC koblitz. The optional parameters are the following.
* 		  163,233,283 for ECC koblitz and 1024, 2048, 3072 for Zp.
* @param numOfThreads
*
*/
OTExtensionMaliciousReceiver::OTExtensionMaliciousReceiver(SocketPartyData serverAddress, int numOts, int numOfThreads, int numBaseOts) : OTExtensionMaliciousBase(1, numBaseOts, numOts) {
	m_connection_manager = new ConnectionManagerClient(1, numOfThreads, serverAddress);
	int nSndVals = 2;
	int wdsize = 1 << (maliciousot::CEIL_LOG2(m_num_base_ots));
	int nblocks = CEIL_DIVIDE(m_num_ots, NUMOTBLOCKS * wdsize);
	int s2ots = nblocks * m_num_base_ots;

	m_sender_key_seeds = new BYTE[AES_KEY_BYTES * m_num_base_ots];//m_security_level.symbits);
	m_receiver_key_seeds_matrix = new BYTE[AES_KEY_BYTES * 2 * s2ots];

	// client connect
	// Create the receiver by passing the local host address.
	
	m_connection_manager->setup_connection();

	// 1st step: pre-compute the PVW base OTs
	precompute_base_ots_receiver();
	assert(nblocks <= NUMOTBLOCKS);
	
	// 2nd step: OT extension step to obtain the base-OTs for the next step
	m_sender = new maliciousot::Mal_OTExtensionSender(nSndVals, m_security_level.symbits,
		m_connection_manager->get_sockets_data(),
		U, m_sender_key_seeds, m_num_base_ots,
		m_num_checks, s2ots, m_sender_seed);

	maliciousot::CBitVector seedA(s2ots * AES_KEY_BITS);
	maliciousot::CBitVector seedB(s2ots * AES_KEY_BITS);

	maliciousot::XORMasking* masking_function = new maliciousot::XORMasking(AES_KEY_BITS);
	m_sender->send(s2ots, AES_KEY_BITS, seedA, seedB, maliciousot::R_OT, 1, masking_function);
	delete masking_function;

	for (int i = 0; i < s2ots; i++) {
		memcpy(m_receiver_key_seeds_matrix + 2 * i * AES_KEY_BYTES,
			seedA.GetArr() + i * AES_KEY_BYTES,
			AES_KEY_BYTES);

		memcpy(m_receiver_key_seeds_matrix + (2 * i + 1) * AES_KEY_BYTES,
			seedB.GetArr() + i * AES_KEY_BYTES,
			AES_KEY_BYTES);
	}

	m_receiver = new maliciousot::Mal_OTExtensionReceiver(nSndVals, m_security_level.symbits,
		m_connection_manager->get_sockets_data(),
		m_receiver_key_seeds_matrix, m_receiver_seed,
		m_num_base_ots, s2ots);

}

BOOL OTExtensionMaliciousReceiver::precompute_base_ots_receiver() {

	int nSndVals = 2;
	BYTE* pBuf = new BYTE[m_num_base_ots * SHA1_BYTES];
	int log_nVals = (int)ceil(log((double)nSndVals) / log((double)2));
	int cnt = 0;

	U.Create(m_num_base_ots * log_nVals, m_receiver_seed, cnt);

	m_baseot_handler->Receiver(nSndVals, m_num_base_ots, U, m_connection_manager->get_socket(0), pBuf);
	//Key expansion
	BYTE* pBufIdx = pBuf;
	for (int i = 0; i<m_num_base_ots; i++) { //80 HF calls for the Naor Pinkas protocol
		memcpy(m_sender_key_seeds + i * AES_KEY_BYTES, pBufIdx, AES_KEY_BYTES);
		pBufIdx += SHA1_BYTES;
	}
	delete[] pBuf;

	return true;
}

/**
* The overloaded function that runs the protocol.<p>
* After the base OT was done by the constructor, call to this function will be optimized and fast, no matter how much OTs there are.
* @param channel Disregarded. This is ignored since the connection is done in the c++ code.
* @param input The input for the receiver specifying the version of the OT extension to run.
* Every call to the transfer function can run a different OT extension version.
*/
shared_ptr<OTBatchROutput> OTExtensionMaliciousReceiver::transfer(OTBatchRInput * input) {

	BYTE version = maliciousot::G_OT;
	//Check if the input is valid. If input is not instance of OTRExtensionInput, throw Exception.
	if (input->getType() != OTBatchRInputTypes::OTExtensionGeneralRInput)
		throw invalid_argument("input should be instance of OTExtensionGeneralRInput");

	/*//If the user gave correlated input, change the version of the OT to correlated.
	if (input instanceof OTExtensionCorrelatedRInput) {
		version = OT_EXTENSION_TYPE_CORRELATED;
	}

	//If the user gave random input, change the version of the OT to random.
	if (input instanceof OTExtensionRandomRInput) {
		version = OT_EXTENSION_TYPE_RANDOM;
	}*/

	auto sigmaArr = ((OTExtensionRInput *)input)->getSigmaArr();
	int numOfOts = ((OTExtensionRInput *)input)->getSigmaArrSize();
	int elementSize = ((OTExtensionRInput *)input)->getElementSize();

	//Run the protocol using the native code in the dll.
	vector<byte> output = runOtAsReceiver(sigmaArr, numOfOts, elementSize, version);

	return make_shared<OTOnByteArrayROutput>(output);
}

vector<byte> OTExtensionMaliciousReceiver::runOtAsReceiver(vector<byte> sigma, int numOfOts, int bitLength, BYTE version) {
	// The masking function with which the values that are sent 
	// in the last communication step are processed
	
	maliciousot::MaskingFunction * masking_function = new maliciousot::XORMasking(bitLength);

	maliciousot::CBitVector choices, response;
	choices.Create(numOfOts);

	//Pre-generate the response vector for the results
	response.Create(numOfOts, bitLength);

	//copy the sigma values received from java
	for (int i = 0; i<numOfOts; i++) {
		choices.SetBit((i / 8) * 8 + 7 - (i % 8), sigma.at(i));
	}

	m_receiver->receive(numOfOts, bitLength, choices, response, version, m_connection_manager->get_num_of_threads(), masking_function);
	
	//prepare the out array
	int sizeResponseInBytes = numOfOts*bitLength / 8;
	vector<byte> output;
	for (int i = 0; i < sizeResponseInBytes; i++) {
		//copy each byte result to out
		output.push_back(response.GetByte(i));
	}

	//free the pointer of choises and reponse
	choices.delCBitVector();
	response.delCBitVector();
	delete masking_function;

	return output;
}
















const char *ConnectionManager::DEFAULT_ADDRESS = "localhost";

/*******************************************************************************
*  Base class for server and client
******************************************************************************/
ConnectionManager::ConnectionManager(int role, int num_of_threads, SocketPartyData party) :
	m_sockets(num_of_threads + 1) { //Number of threads that will be used in OT extension
	m_num_of_threads = num_of_threads;
	m_port = (USHORT)party.getPort();
	m_address = party.getIpAddress().to_string();
	
	m_pid = role;

	//cerr << "ConnectionManager(" << role << "," << num_of_threads << "," << m_address << "," << m_port << ")" << endl;
}

/**
* closes all the open sockets
*/
void ConnectionManager::cleanup() {
	for (int i = 0; i < m_num_of_threads; i++) {
		m_sockets[i].Close();
	}
}

ConnectionManager::~ConnectionManager() {
	cleanup();
}

/*******************************************************************************
*  server class
******************************************************************************/
/**
* ConnectionManagerServer ctors
*/
ConnectionManagerServer::ConnectionManagerServer(int role, int num_of_threads, SocketPartyData party)
	: ConnectionManager(role, num_of_threads, party) {}

/**
* listens and accepts connections on the server
*/
BOOL ConnectionManagerServer::setup_connection() {

	int num_connections = m_num_of_threads + 1;

	//cerr << "ConnectionManagerServer->setup_connection() started." << endl;
	//cout << m_address << endl;
	// try to bind() and then listen
	if ((!m_sockets[0].Socket()) ||
		(!m_sockets[0].Bind(m_port, m_address)) ||
		(!m_sockets[0].Listen())) {
		goto listen_failure;
	}

	for (int i = 0; i<num_connections; i++) { //twice the actual number, due to double sockets for OT
		maliciousot::CSocket sock;

		// try: CSocket sock = accept()
		if (!m_sockets[0].Accept(sock)) {
			cerr << "Error in accept" << endl;
			goto listen_failure;
		}

		// cerr << "Server: accept succeded i = " << i << endl;

		// receive the other side thread id (the first thing that is sent on the socket)
		UINT threadID;
		sock.Receive(&threadID, sizeof(int));

		// cerr << "Server: received threadID = " << threadID << endl;

		// ??
		if (threadID >= num_connections) {
			// cerr << "Server: threadID >= num_connections, num_connections = " << num_connections << endl;
			sock.Close();
			i--;
			continue;
		}

		// locate the socket appropriately
		// cerr << "Server: attaching socket to threadID = " << threadID << endl;
		m_sockets[threadID].AttachFrom(sock);
		sock.Detach();
	}

	//cerr << "ConnectionManagerServer->setup_connection() ended." << endl;

	return TRUE;

listen_failure:
	cerr << "Listen failed" << endl;
	return FALSE;
}

/*******************************************************************************
*  client class
******************************************************************************/

/**
* ConnectionManagerClient ctors
*/
ConnectionManagerClient::ConnectionManagerClient(int role, int num_of_threads, SocketPartyData party)
	: ConnectionManager(role, num_of_threads, party) {}

/**
* initiates a connection (via socket) for each thread on the client
*/
BOOL ConnectionManagerClient::setup_connection() {
	BOOL bFail = FALSE;
	LONG lTO = CONNECT_TIMEO_MILISEC;
	int num_connections = m_num_of_threads + 1;

	//cerr << "ConnectionManagerClient->setup_connection() started." << endl;

	// try to initiate connection for socket k
	for (int k = num_connections - 1; k >= 0; k--) {
		// cerr << "Client: started k = " << k << endl;
		// iterate on retries
		for (int i = 0; i<RETRY_CONNECT; i++) {
			if (!m_sockets[k].Socket()) {
				printf("Socket failure: ");
				goto connect_failure;
			}

			if (m_sockets[k].Connect(m_address, m_port, lTO)) {
				// cerr << "Client:" << k << "connected to (" << m_address << "," << m_port << ")" << endl;

				// send the thread id when connected
				m_sockets[k].Send(&k, sizeof(int));

				// cerr << "Client: sent k = " << k << endl;

				if (k == 0) {
					//cerr << "connected" << endl;
					//cerr << "ConnectionManagerClient->setup_connection() ended." << endl;
					return TRUE;
				}
				else {
					// socket k is connected, breaking the "retries" loop
					// and moving on to the next socket.
					// cerr << "breaking the retries loop" << endl;
					break;
				}

				// TODO: weird: seems to me that this code will never execute!
				// SleepMiliSec(10);
				// m_sockets[k].Close();
			}

			// unable to connect!

			// if all allowed retries failed, server is unavailable
			if (i + 1 == RETRY_CONNECT) {
				goto server_not_available;
			}

			// else, waiting 20 milliseconds before retry
			//cerr << "sleeping 20 milliseconds" << endl;
			SleepMiliSec(20);
		}
	}
server_not_available:
	printf("Server not available: ");
connect_failure:
	cerr << " (" << !m_pid << ") connection failed" << endl;
	return FALSE;
};
