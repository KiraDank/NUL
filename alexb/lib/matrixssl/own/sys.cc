/*
 * (C) 2011 Alexander Boettcher
 *     economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of NUL (NOVA user land).
 *
 * NUL is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * NUL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */

#include <nul/baseprogram.h>
#include <service/logging.h>
#include <service/math.h>
#include <nul/service_timer.h>

#include <matrixssl/matrixsslApi.h>
#include <sampleCerts/privkeySrv.h>
#include <sampleCerts/certSrv.h>

extern "C"
{

static	sslKeys_t		  * keys = NULL;
static  TimerProtocol * service_timer = NULL;
static  uint32          cap_base = 0, cap_size;

int32 nul_ssl_session(void *&ssl) {
  return matrixSslNewServerSession(reinterpret_cast<ssl_t **>(&ssl), keys, NULL); //certDB
}

void nul_ssl_init(void) {
  int32 rc;

  if (matrixSslOpen() < 0) Logging::panic("Ohno!\n");
	if (!keys && matrixSslNewKeys(&keys) < 0) Logging::panic("ohno!\n");

	if ((rc = matrixSslLoadRsaKeysMem(keys, certSrvBuf, sizeof(certSrvBuf),
				   privkeySrvBuf, sizeof(privkeySrvBuf), NULL, 0)) < 0) {
		Logging::printf("failure - ssl - loading certificate material\n");
		matrixSslDeleteKeys(keys);
		matrixSslClose();
	}
}

int32 nul_ssl_len(void * ssl, unsigned char * &buf) {
 int32 len;

 if ((len = matrixSslGetReadbuf(reinterpret_cast<ssl_t *>(ssl), &buf)) <= 0)
	Logging::printf("failure - ssl_len receive buffer unavailable\n");

 return len;
}

int32 nul_ssl_config(int32 transferred, void (*write_out)(uint16 localport, void * out, size_t out_len),
                     void * &appdata, size_t &appdata_len, bool bappdata, uint16 port, void * &ssl_session)
{
  uint32 ubuflen;
  int32 rc, len=0;
  unsigned char * buf;
  ssl_t * ssl = reinterpret_cast<ssl_t *>(ssl_session);

  if (bappdata) {
    rc = matrixSslProcessedData(ssl, &buf, &ubuflen);

    if (appdata && appdata_len > 0) {
      if (rc != MATRIXSSL_SUCCESS) {
        Logging::panic("failure - ssl - processdata error %d\n", rc);
      }
      len = matrixSslEncodeToOutdata(ssl, reinterpret_cast<unsigned char *>(appdata), appdata_len);
      assert(len > 0);
      rc = MATRIXSSL_REQUEST_SEND;
      //matrixSslGetWritebuf
      //matrixSslEncodeWritebuf
      //matrixSslGetOutdata
      //write_out
      //matrixSslSentData
    }
  }
  else
  if ((rc = matrixSslReceivedData(ssl, transferred, &buf, &ubuflen)) < 0)
  	Logging::printf("failure - ssl receive buffer error %d\n", rc);

  loop:

  Logging::printf("        - ssl - next=%d buf=%p buflen=%u len=%d ssl=%p\n", rc, buf, ubuflen, len, ssl);
  switch (rc) {
    case MATRIXSSL_SUCCESS:            // 0 nothing to process, application protocol decides upon next step
      return 0;
    case MATRIXSSL_REQUEST_SEND :      // 1  API produced data to be sent
      assert(buf == 0); //if unequal user data is available
      len = matrixSslGetOutdata(ssl, &buf);
      assert(len > 0);

      write_out(port, buf, len);
      rc = matrixSslSentData(ssl, len);
      goto loop;
      break;
    case MATRIXSSL_APP_DATA:           // 4  App data is avail. to caller 
      assert(buf && ubuflen > 0);
      appdata = buf;
      appdata_len = ubuflen;
      return 1;
    case MATRIXSSL_REQUEST_RECV :      // 2  API requires more data to continue
      return 0; //wait for more data
    case MATRIXSSL_HANDSHAKE_COMPLETE: // 5  Handshake completed 
      return 0; //wait for data
    case MATRIXSSL_REQUEST_CLOSE:      // 3  API indicates clean close is req'd 
      rc  = matrixSslEncodeClosureAlert(ssl);
      if (rc >= 0) {
        if ((len = matrixSslGetOutdata(ssl, &buf)) > 0) {
          Logging::printf("end of live message\n");
          write_out(port, buf, len);
          matrixSslSentData(ssl, len);
        }
      }
      matrixSslDeleteSession(ssl);
      rc  = nul_ssl_session(ssl_session);
      ssl = reinterpret_cast<ssl_t *>(ssl_session);
      assert(rc == 0);
      return -1;
    case MATRIXSSL_RECEIVED_ALERT:     // 6  An alert was received 
      Logging::printf("        - ssl - received alert %s(%x) type=%x \n", 
        (ubuflen > 1 && buf[0] == SSL_ALERT_LEVEL_WARNING) ? "warning" : (buf[0] == SSL_ALERT_LEVEL_FATAL ? "fatal" : "unknown"),
        (ubuflen > 1 && buf) ? buf[0] : 0, (ubuflen > 1 && buf) ? buf[1] : 0);
      if (buf[1] == SSL_ALERT_CLOSE_NOTIFY) {
        matrixSslProcessedData(ssl, &buf, &ubuflen);
        rc = MATRIXSSL_REQUEST_CLOSE;
      } else
        rc = matrixSslProcessedData(ssl, &buf, &ubuflen);
      goto loop;
/*
case PS_FAILURE : //     -1  
case PS_ARG_FAIL : //     -6  Failure due to bad function param 
case PS_PLATFORM_FAIL  -7  Failure as a result of system call error
case PS_MEM_FAIL     -8  Failure to allocate requested memory
case PS_LIMIT_FAIL   -9  Failure on sanity/limit tests
case PS_UNSUPPORTED_FAIL -10 Unimplemented feature error
case PS_DISABLED_FEATURE_FAIL -11 Incorrect #define toggle for feature
case PS_PROTOCOL_FAIL  -12 A protocol error occurred
case PS_TIMEOUT_FAIL   -13 A timeout occurred and MAY be an error
case PS_INTERRUPT_FAIL -14 An interrupt occurred and MAY be an error
case PS_PENDING      -15  In process. Not necessarily an error
*/
    default:
      Logging::printf("failure - ssl - unknown value %d\n", rc);
      rc = MATRIXSSL_REQUEST_CLOSE;
      goto loop;
  }

  return 0;
}

void _psTrace(char *msg)
{
	Logging::printf("%s", msg);
}

/* message should contain one %s, unless value is NULL */
void _psTraceStr(char *message, char *value)
{
	if (value) {
		Logging::printf(message, value);
	} else {
		Logging::printf("%s", message);
	}
}

/* message should contain one %d */
void _psTraceInt(char *message, int32 value)
{
	Logging::printf(message, value);
}

/* message should contain one %p */
void _psTracePtr(char *message, void *value)
{
	Logging::printf(message, value);
}

int32 psGetEntropy(unsigned char *bytes, uint32 size)
{
  Logging::printf("warning - ssl entropy not implemented!\n");
  return size;
}

int osdepTimeOpen(void) {
  TimerProtocol::MessageTime msg;
  if (!cap_size) cap_size = TimerProtocol::CAP_SERVER_PT + Global::hip.cpu_desc_count();
  if (!cap_base) cap_base = alloc_cap_region(cap_size,0); ///XXX not threadsafe
  if (!cap_base) return PS_FAILURE;

  service_timer = new TimerProtocol(cap_base);
  if (!service_timer || service_timer->time(*BaseProgram::myutcb(),msg)) return PS_FAILURE;
  return PS_SUCCESS;
}

void osdepTimeClose(void) {
  if (!service_timer) return;

  service_timer->close(*BaseProgram::myutcb(), cap_size);
  dealloc_cap_region(cap_base, cap_size); //XXX not threadsafe
  cap_base = cap_size = 0;
  delete service_timer;
  service_timer = 0;
}

int osdepEntropyOpen(void) { Logging::printf("warning - osdepentropyopen not implemented!\n"); return PS_SUCCESS;}
void osdepEntropyClose(void) { Logging::printf("warning - osdepentropyclose not implemented!\n");}


void *malloc(size_t size) {
  void * ptr = new char[size];
  if (!ptr) Logging::panic("failure - ssl - malloc call\n");
  return ptr;
}

void free(void *ptr) {
  if (!ptr) Logging::panic("failure - ssl - free call\n");
  delete [] reinterpret_cast<char *>(ptr);
 }

void *realloc(void *ptr, size_t size) {
  assert(ptr);
  assert(size);
  void * newptr = malloc(size);
  Logging::printf("        - ssl - realloc call ptr=%p size=%zu -> newptr=%p\n", ptr, size, newptr);
  memcpy(newptr, ptr, size); //XXX if ptr size increases, during copying we touch memory we should not XXX
  free(ptr);
  return newptr;
}

int32 psGetTime(psTime_t *t)
{
  TimerProtocol::MessageTime msg;
  if (service_timer->time(*BaseProgram::myutcb(),msg)) return PS_FAILURE;
  if (t) *t = msg.wallclocktime;
  Math::div64(msg.wallclocktime, 1000000U);
  Logging::printf("        - psGetTime success %llu\n", msg.wallclocktime);
  return msg.wallclocktime;
}

int32 psCompareTime(psTime_t a, psTime_t b)
{
  Logging::printf("        - psCompareTime\n");
	//Time comparison.  1 if 'a' is less than or equal.  0 if 'a' is greater
  return a <= b;
}

int32 psDiffMsecs(psTime_t then, psTime_t now)
{
  Logging::printf("        - psDiffMsecs\n");
  now = now - then;
  Math::div64(now, 1000U);
  return now;
}

} //EXTERN C
