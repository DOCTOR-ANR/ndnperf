//============================================================================
// Name        : server.cpp
// Author      : MARCHAL Xavier
// Version     : 1.0
// Copyright   : Your copyright notice
// Description : NDN Server in C++
//============================================================================

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/signing-info.hpp>
#include <ndn-cxx/security/sec-tpm-file.hpp>
#include <ndn-cxx/encoding/buffer-stream.hpp>
#include <ndn-cxx/encoding/buffer.hpp>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <boost/asio.hpp>
#include <boost/bind.hpp>

#include <crypto++/rsa.h>
#include <crypto++/dsa.h>
#include <crypto++/eccrypto.h>
#include <crypto++/sha.h>
#include <crypto++/osrng.h>
#include <crypto++/base64.h>
#include <crypto++/files.h>

#include <iostream>
#include <csignal>
#include <ctime>
#include <string>
#include <thread>

#include "blockingconcurrentqueue.h"

using namespace ndn;

#define DEFAULT_THREAD_COUNT std::thread::hardware_concurrency()
#define DEFAULT_CHUNK_SIZE 8192
#define DEFAULT_FRESHNESS 0
#define DEFAULT_SIGNATURE_TYPE -1
#define DEFAULT_RSA_SIGNATURE 2048
#define DEFAULT_ECDSA_SIGNATURE 256

class Server{
private:
	bool cont;
	Face face;
	const char* prefix;
	KeyChain keyChain;
	moodycamel::BlockingConcurrentQueue<std::pair<Interest,std::chrono::steady_clock::time_point>> queue;
	std::thread *threads;
	int *log_vars; // may be less accurate than atomic vars but no sync required
	Block content;
	const int thread_count,payload_size,key_type,key_size;
	const time::milliseconds freshness;
	Name defaultIdentity,identity,keyName,certName;
	void (*signFunction)(Data&);
	Signature sig;
	shared_ptr<PublicKey> pubkey;
	CryptoPP::RSASS<CryptoPP::PKCS1v15, CryptoPP::SHA256>::Signer rsaSigner;
	CryptoPP::ECDSA<CryptoPP::ECP, CryptoPP::SHA256>::Signer ecdsaSigner;

	void gen_random(char *s, const int len) {
    		static const char alphanum[] =
        		"0123456789"
		        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        		"abcdefghijklmnopqrstuvwxyz";

    		for (int i = 0; i < len; ++i) {
        		s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
    		}

    		s[len] = 0;
	}

	void customSign(Data& data){ // you can expect around 10% less time consumption compare to default sign() function (4 threads on bi Xeon E5-2630v3 server)
		data.setSignature(sig);
		EncodingBuffer enc;
		data.wireEncode(enc,true);
		if(keyName == KeyChain::DIGEST_SHA256_IDENTITY)
			data.wireEncode(enc,Block(tlv::SignatureValue, crypto::sha256(enc.buf(),enc.size())));
		else{
			CryptoPP::AutoSeededRandomPool rng;
			OBufferStream os;
			switch(pubkey->getKeyType()){
				case KEY_TYPE_RSA:
					//CryptoPP::RSASS<CryptoPP::PKCS1v15, CryptoPP::SHA256>::Signer rsaSignerCopy = rsaSigner;
					CryptoPP::StringSource(enc.buf(), enc.size(), true, new CryptoPP::SignerFilter(rng, rsaSigner, new CryptoPP::FileSink(os)));
					data.wireEncode(enc,Block(tlv::SignatureValue, os.buf()));
					break;
				case KEY_TYPE_ECDSA:
					CryptoPP::ECDSA<CryptoPP::ECP, CryptoPP::SHA256>::Signer ecdsaSignerCopy = ecdsaSigner; // if not present -> segfault with concurrency
					CryptoPP::StringSource(enc.buf(), enc.size(), true, new CryptoPP::SignerFilter(rng, ecdsaSignerCopy, new CryptoPP::FileSink(os)));
					uint8_t buf[200];
					size_t bufSize = CryptoPP::DSAConvertSignatureFormat(buf, 200, CryptoPP::DSA_DER, os.buf()->buf(), os.buf()->size(), CryptoPP::DSA_P1363);
					shared_ptr<Buffer> sigBuffer = make_shared<Buffer>(buf, bufSize);
					data.wireEncode(enc,Block(tlv::SignatureValue, sigBuffer));
					break;
			}
		}
	}

public:
	Server(const char* prefix, int key_type, int key_size, int thread_count, int freshness, int payload_size):cont(true), prefix(prefix), key_type(key_type), key_size(key_size), thread_count(thread_count), freshness(freshness), payload_size(payload_size){
		if(key_type<0){
			key_type=1;
			key_size=DEFAULT_RSA_SIGNATURE;
		}
		if(key_type>0){ // index of NDN Signature: http://named-data.net/doc/NDN-TLV/current/signature.html
			identity=Name(prefix);
			keyChain.createIdentity(identity);
			defaultIdentity=keyChain.getDefaultIdentity(); // signature take less time with default sign() function (xeon E5-2630v3 with RSA-2048: ptime with default 3500us with certName 4100us, with identity 6500us)
			keyChain.setDefaultIdentity(identity);
			std::cout << "Set default identity to " << identity << " instead of " << defaultIdentity << std::endl;

			std::string digest;
			CryptoPP::SHA256 hash;
			CryptoPP::ByteQueue bytes;
			switch(key_type){
				default:
				case 1:{
					std::cout << "Generating RSA signature with " << key_size << "bits" << std::endl;
					keyName=keyChain.generateRsaKeyPairAsDefault(identity,false,key_size);
					CryptoPP::StringSource src(keyName.toUri(), true, new CryptoPP::HashFilter(hash, new CryptoPP::Base64Encoder(new CryptoPP::StringSink(digest))));
					boost::algorithm::trim(digest);
					std::replace(digest.begin(), digest.end(), '/', '%');
					boost::filesystem::path path = boost::filesystem::path(getenv("HOME")) / ".ndn" / "ndnsec-tpm-file" / (digest + ".pri");

					CryptoPP::FileSource file(path.string().c_str(), true, new CryptoPP::Base64Decoder);
					file.TransferTo(bytes);
					bytes.MessageEnd();
					CryptoPP::RSA::PrivateKey privateRsaKey;
					privateRsaKey.Load(bytes);
					rsaSigner = CryptoPP::RSASS<CryptoPP::PKCS1v15, CryptoPP::SHA256>::Signer(privateRsaKey);
					break;
				}
				case 3:{
					std::cout << "Generating ECDSA signature with " << key_size << "bits" << std::endl;
					keyName=keyChain.generateEcdsaKeyPairAsDefault(identity,false,key_size);
					CryptoPP::StringSource src(keyName.toUri(), true, new CryptoPP::HashFilter(hash, new CryptoPP::Base64Encoder(new CryptoPP::StringSink(digest))));
					boost::algorithm::trim(digest);
					std::replace(digest.begin(), digest.end(), '/', '%');
					boost::filesystem::path path = boost::filesystem::path(getenv("HOME")) / ".ndn" / "ndnsec-tpm-file" / (digest + ".pri");

					CryptoPP::FileSource file(path.string().c_str(), true, new CryptoPP::Base64Decoder);
					file.TransferTo(bytes);
					bytes.MessageEnd();
					CryptoPP::ECDSA<CryptoPP::ECP, CryptoPP::SHA256>::PrivateKey privateEcdsaKey;
					privateEcdsaKey.Load(bytes);
					ecdsaSigner = CryptoPP::ECDSA<CryptoPP::ECP, CryptoPP::SHA256>::Signer(privateEcdsaKey);
					break;
				}
			}
			shared_ptr<IdentityCertificate> certificate=keyChain.selfSign(keyName);
			keyChain.addCertificate(*certificate);
			keyChain.setDefaultCertificateNameForKey(certificate->getName());
			certName=certificate->getName();

			pubkey = keyChain.getPublicKeyFromTpm(certificate->getPublicKeyName());
			SignatureInfo sigInfo = KeyChain::DEFAULT_SIGNING_INFO.getSignatureInfo();
			sigInfo.setSignatureType((tlv::SignatureTypeValue)key_type);
			sigInfo.setKeyLocator(KeyLocator(certificate->getName().getPrefix(-1)));
			sig=Signature(sigInfo);

			Name defaultIdentityName = keyChain.getDefaultIdentity();
	                Name defaultKeyName = keyChain.getDefaultKeyNameForIdentity(identity);
		        Name defaultCertName = keyChain.getDefaultCertificateNameForKey(defaultKeyName);
        		std::cout << "+ Using identity: " << defaultIdentityName << "\n"
	                << "+ Using key: " << defaultKeyName << "\n"
		        << "+ Using certificate: " << defaultCertName << std::endl;
		}else{
			std::cout << "Using SHA-256 signature" << std::endl;
			keyName=KeyChain::DIGEST_SHA256_IDENTITY;
			SignatureInfo sigInfo = KeyChain::DEFAULT_SIGNING_INFO.getSignatureInfo();
			sigInfo.setSignatureType(tlv::DigestSha256);
			sig=Signature(sigInfo);
		}

		threads=new std::thread[thread_count+1]; // N sign threads + 1 display thread
		log_vars=new int[thread_count*4](); // [thread_count][1: payload sum, 2: packet count, 3: qtime, 4: ptime]

		// build once for all the data carried by the packets
		char chararray[payload_size];
		gen_random(chararray,payload_size);
		shared_ptr<Buffer> buf = make_shared<Buffer>(&chararray[0],payload_size);
		content=Block(tlv::Content,buf);
		std::cout << "Payload size = " << content.value_size() << " Bytes" << std::endl;
		std::cout << "Freshness = " << freshness << " ms" << std::endl;
	}

	~Server(){
		cont=false;
		// break the queue wait
		for(int i=0; i<thread_count; ++i)
			queue.enqueue(make_pair(Interest(Name("dummy")),std::chrono::steady_clock::now()));
		for(int i=0; i<thread_count+1; ++i)
			threads[i].join();
		// clean up
		if(key_type>0){
			keyChain.setDefaultIdentity(defaultIdentity);
			std::cout << "Restoring default identity to " << defaultIdentity << ": " << (keyChain.getDefaultIdentity()==defaultIdentity) << std::endl;
			keyChain.deleteCertificate(certName);
			std::cout << "Deleting generated certificate: " << !keyChain.doesCertificateExist(certName) << std::endl;
			keyChain.deleteKey(keyName);
			std::cout << "Deleting generated key: " << !keyChain.doesPublicKeyExist(keyName) << std::endl;
			keyChain.deleteIdentity(identity);
			std::cout << "Deleting generated identity: " << !keyChain.doesIdentityExist(identity) << std::endl;
		}
	}

	void start(){
		face.setInterestFilter(prefix,bind(&Server::on_interest, this, _2),
				bind(&Server::on_register_failed, this));

		for(int i=0;i<thread_count;++i)
			threads[i]=std::thread(&Server::process,this,&log_vars[i*4]);
		std::cout<< "Initialize " << thread_count << " threads" << std::endl;
		threads[thread_count]=std::thread(&Server::display,this);

		face.processEvents();
	}

	void process(int *const i){
		std::pair<Interest,std::chrono::steady_clock::time_point> pair;
		while(cont){
			queue.wait_dequeue(pair);
			auto start = std::chrono::steady_clock::now();
			i[2] += std::chrono::duration_cast<std::chrono::microseconds>(start - pair.second).count();
			//auto it=datas.find(interest.getName().toUri());
			//if(it!=datas.end())face.put(*it->second);
			//else{
                		auto data = make_shared<Data>(pair.first.getName());
				data->setFreshnessPeriod(freshness);
				data->setContent(content);
				//if(key_type>0)keyChain.sign(*data);
				//else keyChain.signWithSha256(*data);
				customSign(*data);
				face.put(*data);
			//}
			i[0] += payload_size;
			++i[1];
			i[3] += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		}
	}

	void display(){
		std::time_t time;
    		char mbstr[28];
		int lv[8]; //[1: payload sum, 2: packet count, 3: qtime, 4: ptime][1: new, 2: last]
		while(cont){
			// accumulate value and compare with last
			for(int i=0; i<8; i+=2){
                                lv[i+1] = -lv[i];
				lv[i] = 0;
                        }
			for(int i=0; i<thread_count; ++i){
				for(int j=0; j<4; ++j){
					lv[2*j] += log_vars[4*i+j];
				}
			}
			for(int i=0; i<8; i+=2){
                                lv[i+1] += lv[i];
			}
			lv[1]>>=8; // in kilobits per sec each 2s (10 + 1 - 3), in decimal 8/(1024*2);
			lv[5]/=lv[3]!=0?lv[3]:-1; // negative value if unusual
			lv[7]/=lv[3]!=0?lv[3]:-1;
			lv[3]>>=1; // per sec each 2s
			time=std::time(NULL);
			std::strftime(mbstr, sizeof(mbstr), "%c - ", std::localtime(&time));
			std::cout << mbstr << lv[1] << " Kbps( " << lv[3] << " pkt/s) - qtime= " << lv[5] << " us, ptime= " << lv[7] << " us" << std::endl;
			std::this_thread::sleep_for(std::chrono::seconds(2));
		}
	}

	void on_interest(const Interest& interest){
		queue.enqueue(make_pair(interest, std::chrono::steady_clock::now()));
	}

	void on_register_failed(){
		exit(1);
	}
};

Server *server;

void signalHandler(int signum){
	std::cout << "\nCatch interruption!" << std::endl;
	delete server;
	exit(0);
}

int main(int argc, char *argv[]) {
	int thread_count=DEFAULT_THREAD_COUNT,
		chunk_size=DEFAULT_CHUNK_SIZE,
		freshness=DEFAULT_FRESHNESS,
		key_type=DEFAULT_SIGNATURE_TYPE,
		key_size=-1;
	const char *prefix="/throughput";
	for(int i=1;i<argc;i+=2){
		switch(argv[i][1]){
		case 'p':
			prefix=argv[i+1];
			break;
		case 's':
			if(atoi(argv[i+1])==0)key_type=0;
			else if(atoi(argv[i+1])==1)key_type=1;
			else if(atoi(argv[i+1])==3)key_type=3;
			break;
		case 'k':
			if(key_type>0){
				if(key_type==1)key_size=atoi(argv[i+1])>=512?atoi(argv[i+1]):DEFAULT_RSA_SIGNATURE;
				else if(key_type==3)key_size=atoi(argv[i+1])>=160?atoi(argv[i+1]):DEFAULT_ECDSA_SIGNATURE;
			}
			break;
		case 't':
			thread_count=atoi(argv[i+1])>=0?atoi(argv[i+1]):DEFAULT_THREAD_COUNT;
			break;
		case 'c':
			chunk_size=atoi(argv[i+1])>0?atoi(argv[i+1]):DEFAULT_CHUNK_SIZE;
			break;
		case 'f':
			freshness=atoi(argv[i+1])>0?atoi(argv[i+1]):DEFAULT_FRESHNESS;
                        break;
		case 'h':
		default:
			std::cout << "usage: ./ndnperfserver [options...]\n"
			<< "\t-p prefix\t(default = /throughput)\n"
			<< "\t-s sign_type\t0=SHA,1=RSA,3=ECDSA (default = 1)\n"
			<< "\t-k key_size\tlimited by the lib, RSA={1024,2048} and ECDSA={256,384}\n"
			<< "\t-t thread_count\t(default = CPU core number)\n"
			<< "\t-c chunk_size\t(default = 8192)\n"
			<< "\t-f freshness\tin milliseconds (default = 0)\n" << std::endl;
			return 0;
		}
	}
	server = new Server(prefix,key_type,key_size,thread_count,freshness,chunk_size);
	signal(SIGINT,signalHandler);
	server->start();
	return 0;
}
