﻿/**
 * SmartCard-HSM PKCS#11 Module
 *
 * Copyright (c) 2017, CardContact Systems GmbH, Minden, Germany
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of CardContact Systems GmbH nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL CardContact Systems GmbH BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file    minidriver.c
 * @author  Andreas Schwier
 * @brief   CSP minidriver frontend for the PKCS#11 token framework
 */

#include <windows.h>
#include "cardmod.h"

#include <common/debug.h>

#include <pkcs11/slot.h>
#include <pkcs11/token.h>
#include <pkcs11/slot-pcsc.h>
#include <pkcs11/object.h>


#define MINIMUM_SUPPORTED_VERSION	7
#define MAXIMUM_SUPPORTED_VERSION	7

// DigestInfo Header encoding in front of hash value
static unsigned char di_sha1[] =   { 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14 };
static unsigned char di_sha256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
static unsigned char di_sha384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
static unsigned char di_sha512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
static unsigned char di_md5[] =    { 0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x05, 0x05, 0x00, 0x04, 0x10 };



/**
 * Mutux callbacks
 */
CK_RV p11CreateMutex(CK_VOID_PTR_PTR ppMutex)
{
	return CKR_OK;
}



CK_RV p11DestroyMutex(CK_VOID_PTR pMutex)
{
	return CKR_OK;
}



CK_RV p11LockMutex(CK_VOID_PTR pMutex)
{
	return CKR_OK;
}



CK_RV p11UnlockMutex(CK_VOID_PTR pMutex)
{
	return CKR_OK;
}



/**
 * Map P11 error codes to CSP error codes
 */
static DWORD mapError(int rc)
{
	switch(rc) {
	case CKR_DEVICE_ERROR:
		return SCARD_E_UNEXPECTED;
	case CKR_MECHANISM_INVALID:
	case CKR_KEY_FUNCTION_NOT_PERMITTED:
		return SCARD_E_UNSUPPORTED_FEATURE;
	case CKR_PIN_INCORRECT:
		return SCARD_W_WRONG_CHV;
	case CKR_PIN_LOCKED:
		return SCARD_W_CHV_BLOCKED;
	case CKR_PIN_LEN_RANGE:
		return SCARD_E_INVALID_PARAMETER;
	case CKR_USER_NOT_LOGGED_IN:
		return SCARD_W_SECURITY_VIOLATION;
	default:
#ifdef DEBUG
		debug("Unmapped error code %lx\n", rc);
#endif
		return SCARD_E_UNEXPECTED;
	}
}



/**
 * Check for removed, replaced cards or shared card handles
 */
static DWORD validateToken(PCARD_DATA pCardData, struct p11Token_t **token)
{
	struct p11Slot_t *slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;
	int rc;
	DWORD dwret;

#ifdef DEBUG
	if (slot->card != pCardData->hScard) {
		debug("hScard has changed.\n");
	}

	if (slot->context != pCardData->hSCardCtx) {
		debug("hSCardCtx has changed.\n");
	}
#endif

	slot->card = pCardData->hScard;
	slot->context = pCardData->hSCardCtx;

	rc = getValidatedToken(slot, token);

	if (rc != CKR_OK) {
		dwret = mapError(rc);
		FUNC_FAILS(dwret, "Obtaining valid token failed");
	}
	return SCARD_S_SUCCESS;
}



/**
 * Copy memory region thereby inverting the byte order
 */
static void copyInverted(PBYTE dst, PBYTE src, DWORD cnt)
{
	src += cnt - 1;
	while (cnt--)
		*dst++ = *src--;
}



/**
 * Determine the number of keys on the device
 */
static int getNumberOfContainers(PCARD_DATA pCardData)
{
	struct p11Slot_t *slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;
	struct p11Object_t *obj = NULL;
	int cnt = 0;

	FUNC_CALLED();

	while (1)	{
		enumerateTokenPrivateObjects(slot->token, &obj);
		if (obj == NULL)
			break;
		cnt++;
	}
	FUNC_RETURNS(cnt);
}



/**
 * Get key for index
 */
static void getKeyForIndex(PCARD_DATA pCardData, int index, struct p11Object_t **pobj)
{
	struct p11Slot_t *slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;
	struct p11Object_t *obj = NULL;

	FUNC_CALLED();

	while (index >= 0)	{
		enumerateTokenPrivateObjects(slot->token, &obj);
		if (obj == NULL)
			break;
		index--;
	}

	*pobj = obj;
}



#define bcddigit(x) ((x) >= 10 ? 'a' - 10 + (x) : '0' + (x))

/*
 * Convert a string of bytes in BCD coding to a string of hexadecimal char.
 * Caller must allocate a buffer with len * 2 + 1 characters.
 *
 */
static void decodeBCDString(unsigned char *Inbuff, int len, char *Outbuff)
{
	while (len--) {
		*Outbuff++ = bcddigit(*Inbuff >> 4);
		*Outbuff++ = bcddigit(*Inbuff & 15);
		Inbuff++;
	}
	*Outbuff++ = '\0';
}



/**
 * Convert a 16 byte binary GUID to the 8-4-4-4-12 format.
 * The caller must allocate a sufficient long buffer with at least 37 character.
 */
static void GUIDtoString(unsigned char *guid, char *outbuff)
{
	decodeBCDString(guid, 4, outbuff);
	outbuff += 8;
	*outbuff++ = '-';
	guid += 4;

	decodeBCDString(guid, 2, outbuff);
	outbuff += 4;
	*outbuff++ = '-';
	guid += 2;

	decodeBCDString(guid, 2, outbuff);
	outbuff += 4;
	*outbuff++ = '-';
	guid += 2;

	decodeBCDString(guid, 2, outbuff);
	outbuff += 4;
	*outbuff++ = '-';
	guid += 2;

	decodeBCDString(guid, 6, outbuff);
}



/**
 * Check if filename is valid
 */
static DWORD checkFileName(LPSTR name)
{
	size_t s, i;

	s = strlen(name);

	if ((s < 1) || (s > 8))
		return SCARD_E_INVALID_PARAMETER;

	for (i = 0; i < s; i++) {
		if (!isprint(name[i]))
			return SCARD_E_INVALID_PARAMETER;
	}
	return SCARD_S_SUCCESS;
}



/**
 * Determine and encode a GUID for the referenced key
 *
 * If CKA_ID is sufficiently long (>=16), then the first 16 bytes are used as GUID. If CKA_ID
 * is to short, then the token serial number is xored with CKA_ID to create a static GUID.
 */
static void encodeGUID(PCARD_DATA pCardData, struct p11Object_t *obj, PCONTAINER_MAP_RECORD cont)
{
	struct p11Slot_t *slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;
	struct p11Attribute_t *attr;
	unsigned char idscr[16],*spo,*dpo,*id;
	char scr[MAX_CONTAINER_NAME_LEN + 1];
	size_t i;

	findAttribute(obj, CKA_ID, &attr);
		
	if (attr->attrData.ulValueLen < 16) {
		memcpy(idscr, slot->token->info.serialNumber, 16);
		spo = idscr + 16 - attr->attrData.ulValueLen;
		dpo = attr->attrData.pValue;

		// XOR Serialnumber and Key Id
		for (i = 0; i < attr->attrData.ulValueLen; i++) {
			*spo = *spo ^ *dpo;
			spo++;
			dpo++;
		}
		id = idscr;
	} else {
		id = attr->attrData.pValue;
	}

	GUIDtoString(id, scr);

	mbstowcs_s(&i, cont->wszGuid, MAX_CONTAINER_NAME_LEN + 1, scr, strlen(scr));
}



/**
 * Dynamically encode the CMapFile that Windows uses to map GUIDs to key containers.
 */
static DWORD encodeCMapFile(PCARD_DATA pCardData, PCONTAINER_MAP_RECORD cont, int nofc)
{
	struct p11Slot_t *slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;
	struct p11Object_t *obj = NULL;
	int i;

	FUNC_CALLED();

	for (i = 0; i < nofc; i++) {
		enumerateTokenPrivateObjects(slot->token, &obj);
		if (obj == NULL)
			break;

		encodeGUID(pCardData, obj, cont);
		cont->bFlags = CONTAINER_MAP_VALID_CONTAINER;

		if (!i)
			cont->bFlags |= CONTAINER_MAP_DEFAULT_CONTAINER;

		cont->wKeyExchangeKeySizeBits = obj->keysize;
		cont->wSigKeySizeBits = 0;
		cont++;
	}

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD CardQueryPINInfo(__in PCARD_DATA pCardData,
	__in DWORD dwFlags,
	__inout PPIN_INFO  pPINInfo)
{
	struct p11Slot_t *slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,dwFlags=%lu,pPINInfo=%p)\n", pCardData, dwFlags, pPINInfo);
#endif

	if (pPINInfo->dwVersion > PIN_INFO_CURRENT_VERSION)
		FUNC_FAILS(ERROR_REVISION_MISMATCH, "Structure version mismatch");

	pPINInfo->dwVersion = PIN_INFO_CURRENT_VERSION;
	if (dwFlags == ROLE_USER) {
		pPINInfo->PinType = slot->token->info.flags & CKF_PROTECTED_AUTHENTICATION_PATH ? ExternalPinType : AlphaNumericPinType;
		pPINInfo->PinPurpose = PrimaryCardPin;
		pPINInfo->PinCachePolicy.dwVersion = PIN_CACHE_POLICY_CURRENT_VERSION;
		pPINInfo->PinCachePolicy.dwPinCachePolicyInfo = 0;
		pPINInfo->PinCachePolicy.PinCachePolicyType = PinCacheNormal;
		pPINInfo->dwChangePermission = CREATE_PIN_SET(ROLE_USER);
		pPINInfo->dwUnblockPermission = PIN_SET_NONE;
	} else {
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "Unspported ROLE");
	}

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardDeleteContext(__inout PCARD_DATA  pCardData)
{
	struct p11Slot_t *slot;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p)\n", pCardData);
#endif

	if (pCardData == NULL)		// CMR_48
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;
	freeToken(slot->token);

	if (pCardData->pvVendorSpecific != NULL) {
		pCardData->pfnCspFree(pCardData->pvVendorSpecific);
		pCardData->pvVendorSpecific = NULL;
	}

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardAuthenticatePin(__in PCARD_DATA pCardData,
	__in LPWSTR pwszUserId,
	__in_bcount(cbPin) PBYTE pbPin,
	__in DWORD cbPin,
	__out_opt PDWORD pcAttemptsRemaining)
{
	PIN_ID pinId;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,pwszUserId='%S',pbPin=%p,cbPin=%lu,pcAttemptsRemaining=%p )\n", pCardData, pwszUserId, pbPin, cbPin, pcAttemptsRemaining);
#endif
	
	if (pwszUserId == NULL)		// CMR_53
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pwszUserId validation failed");

	if (wcscmp(pwszUserId, wszCARD_USER_USER) == 0)	{
		pinId = ROLE_USER;
	} else if (wcscmp(pwszUserId, wszCARD_USER_ADMIN) == 0) {
		pinId = ROLE_ADMIN;
	} else {
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pwszUserId invalid value");
	}

	return CardAuthenticateEx(pCardData, pinId, 0, pbPin, cbPin, NULL, NULL, pcAttemptsRemaining);
}



static DWORD WINAPI CardAuthenticateEx(__in PCARD_DATA pCardData,
	__in   PIN_ID PinId,
	__in   DWORD dwFlags,
	__in_bcount(cbPinData) PBYTE pbPinData,
	__in   DWORD cbPinData,
	__deref_opt_out_bcount(*pcbSessionPin) PBYTE *ppbSessionPin,
	__out_opt PDWORD pcbSessionPin,
	__out_opt PDWORD pcAttemptsRemaining)
{
	struct p11Token_t *token;
	DWORD dwret;
	int rc;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,PinId=%d,dwFlags=%lu,pbPinData=%p,cbPinData=%lu,ppbSessionPin=%p,pcbSessionPin=%p,pcAttemptsRemaining=%p )\n", pCardData, PinId, dwFlags, pbPinData, cbPinData, ppbSessionPin, pcbSessionPin, pcAttemptsRemaining);
#endif
	
	if (pCardData == NULL)			// CMR_71
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (PinId != ROLE_USER)			// CMR_72
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "PinId validation failed");

	if (dwFlags & ~(CARD_AUTHENTICATE_GENERATE_SESSION_PIN | CARD_AUTHENTICATE_SESSION_PIN | CARD_PIN_SILENT_CONTEXT))		// CMR_74
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

	if (cbPinData > 16)				// CMR_75
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "cbPinData exceeds range");

	if (dwFlags & (CARD_AUTHENTICATE_GENERATE_SESSION_PIN | CARD_AUTHENTICATE_SESSION_PIN))		// CMR_66
		FUNC_FAILS(SCARD_E_UNSUPPORTED_FEATURE, "Session PIN not supported");

	dwret = validateToken(pCardData, &token);
	if (dwret != SCARD_S_SUCCESS)
		FUNC_FAILS(dwret, "Could not obtain fresh token reference");

	rc = logIn(token->slot, CKU_USER, pbPinData, cbPinData);

	if (rc != CKR_OK) {
		if (pcAttemptsRemaining != NULL)
			*pcAttemptsRemaining = (DWORD)token->pinTriesLeft;

		dwret = mapError(rc);
		FUNC_FAILS(dwret, "PIN verification failed");
	}

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardDeauthenticate(__in PCARD_DATA pCardData,
	__in LPWSTR pwszUserId,
	__in DWORD dwFlags)
{
	PIN_ID pinId;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,pwszUserId='%S'dwFlags=%lu )\n", pCardData, pwszUserId, dwFlags);
#endif
	
	if (pwszUserId == NULL)		// CMR_53
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pwszUserId validation failed");

	if (wcscmp(pwszUserId, wszCARD_USER_USER) == 0)	{
		pinId = CREATE_PIN_SET(ROLE_USER);
	} else if (wcscmp(pwszUserId, wszCARD_USER_ADMIN) == 0) {
		pinId = CREATE_PIN_SET(ROLE_ADMIN);
	} else {
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pwszUserId invalid value");
	}

	return CardDeauthenticateEx(pCardData, pinId, dwFlags);
}



static DWORD WINAPI CardDeauthenticateEx(__in PCARD_DATA pCardData,
	__in PIN_SET PinId,
	__in DWORD dwFlags)
{
	struct p11Token_t *token;
	DWORD dwret;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,PinId=%lx,dwFlags=%lu )\n", pCardData, PinId, dwFlags);
#endif

	if (pCardData == NULL)		// CMR_128
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (dwFlags != 0)			// CMR_129
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

	// CMR_130
	if (PinId & ~(CREATE_PIN_SET(ROLE_EVERYONE) | CREATE_PIN_SET(ROLE_USER) | CREATE_PIN_SET(ROLE_ADMIN) | CREATE_PIN_SET(3) | CREATE_PIN_SET(4) | CREATE_PIN_SET(5) | CREATE_PIN_SET(6) | CREATE_PIN_SET(7)))
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "PinId validation failed");

	dwret = validateToken(pCardData, &token);
	if (dwret != SCARD_S_SUCCESS)
		FUNC_FAILS(dwret, "Could not obtain fresh token reference");

	logOut(token->slot);

// Returning SCARD_E_UNSUPPORTED_FEATURE causes a card reset, which is not correctly
// reflected if multitple CardAcquireContexts are in use. Each context maintains it's
// own state of selected application, but is not notified of the reset.
// Disabling the card reset leaves the card in an authenticated state until
// it is powered down.
//
// This is not an issue for the SmartCard-HSM which supports a logout mechanism
//
//	if (strncmp((char *)token->info.model, "SmartCard-HSM", 13)) {
//		FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
//	}

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static void enumerateX509CACertificates(struct p11Token_t *token, struct p11Object_t **obj)
{
	struct p11Attribute_t *attr;

	while (1)	{
		enumerateTokenPublicObjects(token, obj);
		if (*obj == NULL)
			break;

		if (findAttribute(*obj, CKA_CLASS, &attr) < 0) {
			continue;
		}

		if (*(CK_OBJECT_CLASS *)attr->attrData.pValue != CKO_CERTIFICATE) {
			continue;
		}

		if (findAttribute(*obj, CKA_CERTIFICATE_TYPE, &attr) < 0) {
			continue;
		}

		if (*(CK_CERTIFICATE_TYPE *)attr->attrData.pValue != CKC_X_509) {
			continue;
		}

		if (findAttribute(*obj, CKA_CERTIFICATE_CATEGORY, &attr) < 0) {
			continue;
		}

		if (*(CK_ULONG *)attr->attrData.pValue != 2) {
			continue;
		}
		return;
	}
}



static DWORD encodeMSROOTSFile(PCARD_DATA pCardData, PBYTE *ppbData, PDWORD pcbData)
{
	struct p11Slot_t *slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;
	struct p11Object_t *obj = NULL;
	struct p11Attribute_t *attr;
	HCERTSTORE hCertStore = NULL;
	PCCERT_CONTEXT cert = NULL;
	CERT_BLOB certBlob = { 0, NULL };
	int cnt = 0;

	FUNC_CALLED();

	hCertStore = CertOpenStore(CERT_STORE_PROV_MEMORY, X509_ASN_ENCODING, (HCRYPTPROV_LEGACY)NULL, 0, NULL);
	
	if (hCertStore == NULL)
		FUNC_FAILS(SCARD_E_UNEXPECTED, "CertOpenStore() failed");

	while (1)	{
		enumerateX509CACertificates(slot->token, &obj);

		if (obj == NULL)
			break;

		if (findAttribute(obj, CKA_VALUE, &attr) < 0) {
			continue;
		}

		cert = CertCreateCertificateContext(X509_ASN_ENCODING, (PBYTE)attr->attrData.pValue, (DWORD)attr->attrData.ulValueLen);

		if (cert == NULL) {
#ifdef DEBUG
			debug("Unable to decode certificate %i using CertCreateCertificateContext\n", cnt);
#endif
			continue;
		}

		CertAddCertificateContextToStore(hCertStore, cert, CERT_STORE_ADD_REPLACE_EXISTING, NULL);
		CertFreeCertificateContext(cert);

		cnt++;
	}

	if (!CertSaveStore(hCertStore, PKCS_7_ASN_ENCODING|X509_ASN_ENCODING, CERT_STORE_SAVE_AS_PKCS7, CERT_STORE_SAVE_TO_MEMORY, &certBlob, 0)) {
		CertCloseStore(hCertStore, CERT_CLOSE_STORE_FORCE_FLAG);
		FUNC_FAILS(SCARD_E_UNEXPECTED, "CertSaveStore() failed");
	}

	*pcbData = certBlob.cbData;
	*ppbData = (PBYTE)pCardData->pfnCspAlloc(certBlob.cbData);

	if (*ppbData == NULL) {
		CertCloseStore(hCertStore, CERT_CLOSE_STORE_FORCE_FLAG);
		FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");
	}

	certBlob.pbData = *ppbData;

	if (!CertSaveStore(hCertStore, PKCS_7_ASN_ENCODING|X509_ASN_ENCODING, CERT_STORE_SAVE_AS_PKCS7, CERT_STORE_SAVE_TO_MEMORY, &certBlob, 0)) {
		pCardData->pfnCspFree(*ppbData);
		CertCloseStore(hCertStore, CERT_CLOSE_STORE_FORCE_FLAG);
		FUNC_FAILS(SCARD_E_UNEXPECTED, "CertSaveStore() failed");
	}

	CertCloseStore(hCertStore, CERT_CLOSE_STORE_FORCE_FLAG);

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD readCertificate(PCARD_DATA pCardData, int iContainerIndex, PBYTE *ppbData, PDWORD pcbData)
{
	struct p11Slot_t *slot;
	struct p11Object_t *p11prikey, *p11cert;
	struct p11Attribute_t *attr;

	FUNC_CALLED();

	p11prikey = NULL;
	getKeyForIndex(pCardData, iContainerIndex, &p11prikey);

	if (p11prikey == NULL)
		FUNC_FAILS(SCARD_E_FILE_NOT_FOUND, "iContainerIndex invalid");

	if (findAttribute(p11prikey, CKA_ID, &attr) < 0)
		FUNC_FAILS(SCARD_E_UNEXPECTED, "Could not find attribute CKA_ID in private key");

	slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;
	if (findMatchingTokenObjectById(slot->token, CKO_CERTIFICATE, (unsigned char *)attr->attrData.pValue, attr->attrData.ulValueLen, &p11cert) != CKR_OK)
		FUNC_FAILS(SCARD_E_UNEXPECTED, "Could not find matching certificate");

	if (findAttribute(p11cert, CKA_VALUE, &attr) < 0)
		FUNC_FAILS(SCARD_E_UNEXPECTED, "Could not find CKA_VALUE in certificate");

	*pcbData = attr->attrData.ulValueLen;
	*ppbData = (PBYTE)pCardData->pfnCspAlloc(*pcbData);

	if (*ppbData == NULL)
		FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");

	CopyMemory(*ppbData, attr->attrData.pValue, *pcbData);

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardReadFile(__in PCARD_DATA pCardData,
	__in_opt LPSTR pszDirectoryName,
	__in LPSTR pszFileName,
	__in DWORD dwFlags,
	__deref_out_bcount_opt(*pcbData) PBYTE *ppbData,
	__out PDWORD pcbData)
{
	struct p11Slot_t *slot;
	int containers,i;
	DWORD dwret;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,pszDirectoryName='%s',pszFileName='%s',dwFlags=%lu,ppbData=%p,pcbData=%p )\n", pCardData, NULLSTR(pszDirectoryName), NULLSTR(pszFileName), dwFlags, ppbData, pcbData);
#endif

	if (pCardData == NULL)		// CMR_217
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if ((pszFileName == NULL) || (*pszFileName == 0))		// CMR_218
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pszFileName validation failed");

	if (ppbData == NULL)		// CMR_219
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "ppbData validation failed");

	if (pcbData == NULL)		// CMR_220
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pcbData validation failed");

	if (pszDirectoryName != NULL) {
		dwret = checkFileName(pszDirectoryName);		// CMR_221 / CMR_222
		if (dwret != SCARD_S_SUCCESS)
			FUNC_FAILS(dwret, "pszDirectoryName validation failed");

		if (_stricmp(pszDirectoryName, szBASE_CSP_DIR))		// CMR_223
			FUNC_FAILS(SCARD_E_DIR_NOT_FOUND, "pszDirectoryName unknown value");
	}

	dwret = checkFileName(pszFileName);		// CMR_224 / CMR_225
	if (dwret != SCARD_S_SUCCESS)
		FUNC_FAILS(dwret, "pszFileName validation failed");

	if (dwFlags != 0)			// CMR_227
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

	slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;

	if (pszDirectoryName == NULL) {			// ROOT
		if (!_stricmp(pszFileName, szCARD_IDENTIFIER_FILE)) {
			*pcbData = 16;
			*ppbData = (PBYTE)pCardData->pfnCspAlloc(*pcbData);

			if (*ppbData == NULL)
				FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");

			memcpy(*ppbData, slot->token->info.serialNumber, *pcbData);

		} else if (!_stricmp(pszFileName, szCACHE_FILE)) {
			CARD_CACHE_FILE_FORMAT cache;

			memset(&cache, 0, sizeof(cache));
			*pcbData = sizeof(cache);
			*ppbData = (PBYTE)pCardData->pfnCspAlloc(*pcbData);

			if (*ppbData == NULL)
				FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");

			memcpy(*ppbData, &cache, *pcbData);

		} else if (!_stricmp(pszFileName, "cardapps")) {
			CHAR apps[8] = { 'm', 's', 'c', 'p', 0, 0, 0, 0 };

			*pcbData = sizeof(apps);
			*ppbData = (PBYTE)pCardData->pfnCspAlloc(*pcbData);

			if (*ppbData == NULL)
				FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");

			memcpy(*ppbData, apps, *pcbData);

		} else {
			FUNC_FAILS(SCARD_E_FILE_NOT_FOUND, "pszFileName unknown value");
		}
	} else {								// MSCP
		if (!_stricmp(pszFileName, szCONTAINER_MAP_FILE)) {
			containers = getNumberOfContainers(pCardData);
			*pcbData = containers * sizeof(CONTAINER_MAP_RECORD);
			*ppbData = (PBYTE)pCardData->pfnCspAlloc(*pcbData);

			if (*ppbData == NULL)
				FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");

			dwret = encodeCMapFile(pCardData, (PCONTAINER_MAP_RECORD)*ppbData, containers);

			if (dwret != SCARD_S_SUCCESS)
				FUNC_FAILS(dwret, "Can't encode cmapfile");

		} else if (!_stricmp(pszFileName, szROOT_STORE_FILE)) {
			dwret = encodeMSROOTSFile(pCardData, ppbData, pcbData);

			if (dwret != SCARD_S_SUCCESS)
				FUNC_FAILS(dwret, "Can't encode cmapfile");

		} else if (!_strnicmp(pszFileName, szUSER_KEYEXCHANGE_CERT_PREFIX, 3)) {
			i = atoi(pszFileName + 3);
			dwret = readCertificate(pCardData, i, ppbData, pcbData);

			if (dwret != SCARD_S_SUCCESS)
				FUNC_FAILS(dwret, "Can't read certificate");
		} else {
			FUNC_FAILS(SCARD_E_FILE_NOT_FOUND, "pszFileName unknown value");
		}
	}
	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardGetFileInfo(__in PCARD_DATA pCardData,
	__in_opt LPSTR pszDirectoryName,
	__in LPSTR pszFileName,
	__inout PCARD_FILE_INFO pCardFileInfo)
{
	PBYTE bp;
	DWORD bplen, dwret;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,pszDirectoryName='%s',pszFileName='%s',pCardFileInfo=%p )\n", pCardData, NULLSTR(pszDirectoryName), NULLSTR(pszFileName), pCardFileInfo);
#endif

	if (pCardFileInfo == NULL)		// CMR_251
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (pCardFileInfo->dwVersion > CARD_FILE_INFO_CURRENT_VERSION)	// CMR_260
		FUNC_FAILS(ERROR_REVISION_MISMATCH, "Structure version mismatch");

	pCardFileInfo->dwVersion = CARD_FILE_INFO_CURRENT_VERSION;

	bp = NULL;
	dwret = CardReadFile(pCardData, pszDirectoryName, pszFileName, 0, &bp, &bplen);

	if (bp != NULL)
		pCardData->pfnCspFree(bp);

	if (dwret != SCARD_S_SUCCESS)
		FUNC_FAILS(dwret, "Could no acquire file content failed");

	pCardFileInfo->cbFileSize = bplen;
	pCardFileInfo->AccessCondition = EveryoneReadUserWriteAc;

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardEnumFiles(__in PCARD_DATA pCardData,
	__in_opt LPSTR pszDirectoryName,
	__deref_out_ecount(*pdwcbFileName) LPSTR *pmszFileNames,
	__out LPDWORD pdwcbFileName,
	__in DWORD dwFlags)
{
	static BYTE rootFiles[] = { 'c','a','r','d','i','d',0,'c','a','r','d','c','f',0,'c','a','r','d','a','p','p','s',0,0 };
	static BYTE mscpFiles[] = { 'c','m','a','p','f','i','l','e',0,'m','s','r','o','o','t','s',0 };
	LPSTR po;
	int containers,i;
	DWORD dwret;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,pszDirectoryName='%s',pmszFileNames=%p,pdwcbFileName=%p,dwFlags=%lu )\n", pCardData, NULLSTR(pszDirectoryName), pmszFileNames, pdwcbFileName, dwFlags);
#endif

	if (pCardData == NULL)		// CMR_300
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (pmszFileNames == NULL)		// CMR_301
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pmszFileNames validation failed");

	if (pdwcbFileName == NULL)		// CMR_302
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pdwcbFileName validation failed");

	if (pszDirectoryName != NULL) {
		dwret = checkFileName(pszDirectoryName);		// CMR_303 / CMR_304
		if (dwret != SCARD_S_SUCCESS)
			FUNC_FAILS(dwret, "pszDirectoryName validation failed");

		if (_stricmp(pszDirectoryName, szBASE_CSP_DIR))		// CMR_305
			FUNC_FAILS(SCARD_E_DIR_NOT_FOUND, "pszDirectoryName unknown value");
	}

	if (dwFlags != 0)			// CMR_306
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

	if (pszDirectoryName == NULL) {
		*pdwcbFileName = sizeof(rootFiles);
		*pmszFileNames = (LPSTR)pCardData->pfnCspAlloc(*pdwcbFileName);

		if (*pmszFileNames == NULL)
			FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");

		memcpy(*pmszFileNames, rootFiles, *pdwcbFileName);
	} else {
		containers = getNumberOfContainers(pCardData);

		*pdwcbFileName = sizeof(mscpFiles) + containers * 6 + 1;
		*pmszFileNames = (LPSTR)pCardData->pfnCspAlloc(*pdwcbFileName);

		if (*pmszFileNames == NULL)
			FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");

		memcpy(*pmszFileNames, mscpFiles, *pdwcbFileName);

		po = *pmszFileNames + sizeof(mscpFiles);
		for (i = 0; i < containers; i++) {
			sprintf_s(po, 6, "kxc%02i", i);
			po += 6;
		}
		*po++ = 0;
	}

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardQueryFreeSpace(__in PCARD_DATA pCardData, __in DWORD dwFlags,
	__inout PCARD_FREE_SPACE_INFO pCardFreeSpaceInfo)
{
	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,dwFlags=%lu,pCardFreeSpaceInfo=%p)\n", pCardData, dwFlags, pCardFreeSpaceInfo);
#endif

	if (pCardData == NULL)		// CMR_311
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (pCardFreeSpaceInfo == NULL)		// CMR_312
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardFreeSpaceInfo validation failed");

	if (dwFlags != 0)		// CMR_313
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

	if (pCardFreeSpaceInfo->dwVersion > CARD_FREE_SPACE_INFO_CURRENT_VERSION)		// CMR_314
		FUNC_FAILS(ERROR_REVISION_MISMATCH, "Structure version mismatch");

	pCardFreeSpaceInfo->dwVersion = CARD_FREE_SPACE_INFO_CURRENT_VERSION;
	pCardFreeSpaceInfo->dwBytesAvailable = 0;
	pCardFreeSpaceInfo->dwKeyContainersAvailable = 0;
	pCardFreeSpaceInfo->dwMaxKeyContainers = getNumberOfContainers(pCardData);

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardQueryCapabilities(__in PCARD_DATA pCardData,
	__inout PCARD_CAPABILITIES  pCardCapabilities)
{
	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,pCardCapabilities=%p)\n", pCardData, pCardCapabilities);
#endif

	if (pCardData == NULL)		// CMR_318
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (pCardCapabilities == NULL)		// CMR_319
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardCapabilities validation failed");

	if (pCardCapabilities->dwVersion > CARD_CAPABILITIES_CURRENT_VERSION)	// CMR_320
		FUNC_FAILS(ERROR_REVISION_MISMATCH, "Structure version mismatch");

	pCardCapabilities->dwVersion = CARD_CAPABILITIES_CURRENT_VERSION;
	pCardCapabilities->fCertificateCompression = TRUE;
	pCardCapabilities->fKeyGen = FALSE;

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD encodeRSAPublicKey(PCARD_DATA pCardData, unsigned char *modulus, size_t moduluslen, PBYTE *pblob, DWORD *pbloblen )
{
	DWORD bloblen = sizeof(BLOBHEADER) + sizeof(RSAPUBKEY) + moduluslen;
	PBYTE blob = (PBYTE)pCardData->pfnCspAlloc(bloblen);
	BLOBHEADER *bh;
	RSAPUBKEY *rsa;

	if (blob == NULL)
		FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");

	bh = (BLOBHEADER *)blob;
	bh->bType = PUBLICKEYBLOB;
	bh->bVersion = CUR_BLOB_VERSION;
	bh->reserved = 0;
	bh->aiKeyAlg = CALG_RSA_KEYX;

	rsa = (RSAPUBKEY *)(blob + sizeof(BLOBHEADER));
	rsa->magic = 0x31415352;
	rsa->bitlen = moduluslen << 3;
	rsa->pubexp = 65537;

	copyInverted(blob + sizeof(BLOBHEADER) + sizeof(RSAPUBKEY), modulus, moduluslen);

	*pblob = blob;
	*pbloblen = bloblen;

	return SCARD_S_SUCCESS;
}



static DWORD encodeECCPublicKey(PCARD_DATA pCardData, struct p11Object_t *p11pubkey, PBYTE *pblob, DWORD *pbloblen )
{
	static BYTE primeP256r1[] = { 0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07 };
	struct p11Attribute_t *curveattr, *pointattr;
	BCRYPT_ECCKEY_BLOB *ecc;

	if (findAttribute(p11pubkey, CKA_EC_PARAMS, &curveattr) < 0)
		FUNC_FAILS(SCARD_E_UNEXPECTED, "Could not find attribute CKA_EC_PARAMS in public key");

	if (memcmp(primeP256r1, curveattr->attrData.pValue, sizeof(primeP256r1)))
		FUNC_FAILS(SCARD_E_UNEXPECTED, "Unsupported curve");

	if (findAttribute(p11pubkey, CKA_EC_POINT, &pointattr) < 0)
		FUNC_FAILS(SCARD_E_UNEXPECTED, "Could not find attribute CKA_EC_POINT in public key");

	*pbloblen = sizeof(BCRYPT_ECCKEY_BLOB) + 64;
	*pblob = (PBYTE)pCardData->pfnCspAlloc(*pbloblen);

	if (*pblob == NULL)
		FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");

	ecc = (BCRYPT_ECCKEY_BLOB *)(*pblob);

	ecc->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
	ecc->cbKey = 0x40;

	memcpy((*pblob) + sizeof(BCRYPT_ECCKEY_BLOB), (PBYTE)pointattr->attrData.pValue + 3, 0x40);

	return SCARD_S_SUCCESS;
}



static DWORD WINAPI CardGetContainerInfo(__in PCARD_DATA pCardData, __in BYTE bContainerIndex, __in DWORD dwFlags,
	__inout PCONTAINER_INFO pContainerInfo)
{
	struct p11Slot_t *slot;
	struct p11Object_t *p11prikey, *p11pubkey;
	struct p11Attribute_t *attr;
	DWORD dwret;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,bContainerIndex=%d,pContainerInfo=%p)\n", pCardData, bContainerIndex, pContainerInfo);
#endif

	if (pCardData == NULL)		// CMR_377
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (pContainerInfo == NULL)		// CMR_378
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pContainerInfo validation failed");

	if (dwFlags != 0)		// CMR_380
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

	if (pContainerInfo->dwVersion > CONTAINER_INFO_CURRENT_VERSION)	// CMR_381
		FUNC_FAILS(ERROR_REVISION_MISMATCH, "Structure version mismatch");

	p11prikey = NULL;
	getKeyForIndex(pCardData, (int)bContainerIndex, &p11prikey);

	if (p11prikey == NULL)		// CMR_379
		FUNC_FAILS(SCARD_E_NO_KEY_CONTAINER, "bContainerIndex invalid");

	pContainerInfo->dwVersion = CONTAINER_INFO_CURRENT_VERSION;
	pContainerInfo->dwReserved = 0;
	pContainerInfo->pbSigPublicKey = NULL;
	pContainerInfo->cbSigPublicKey = 0;
	pContainerInfo->pbKeyExPublicKey = NULL;
	pContainerInfo->cbKeyExPublicKey = 0;

	if (findAttribute(p11prikey, CKA_ID, &attr) < 0)
		FUNC_FAILS(SCARD_E_UNEXPECTED, "Could not find attribute CKA_ID in private key");

	slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;
	if (findMatchingTokenObjectById(slot->token, CKO_PUBLIC_KEY, (unsigned char *)attr->attrData.pValue, attr->attrData.ulValueLen, &p11pubkey) != CKR_OK)
		FUNC_FAILS(SCARD_E_UNEXPECTED, "Could not find matching public key");

	if (findAttribute(p11pubkey, CKA_MODULUS, &attr) >= 0) {
		dwret = encodeRSAPublicKey(pCardData, (PBYTE)attr->attrData.pValue, attr->attrData.ulValueLen, &pContainerInfo->pbKeyExPublicKey, &pContainerInfo->cbKeyExPublicKey);
	} else {
		dwret = encodeECCPublicKey(pCardData, p11pubkey, &pContainerInfo->pbKeyExPublicKey, &pContainerInfo->cbKeyExPublicKey);
	}

	if (dwret != SCARD_S_SUCCESS)
		FUNC_FAILS(dwret, "Public key encoding failed");

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardRSADecrypt(__in PCARD_DATA pCardData,
	__inout PCARD_RSA_DECRYPT_INFO  pInfo)

{
	struct p11Token_t *token;
	struct p11Object_t *p11prikey;
	struct p11Attribute_t *attr;
	unsigned char cryptogram[512], plain[512];
	CK_MECHANISM mech;
	CK_KEY_TYPE keytype;
	CK_ULONG plainlen;
	PBYTE pp;
	DWORD dwret, dwlen;
	int rc;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,pInfo=%p)\n", pCardData, pInfo);
#endif

	if (pCardData == NULL)		// CMR_413
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (pInfo == NULL)			// CMR_414
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pInfo validation failed");

#ifdef DEBUG
	debug(" pInfo(dwVersion=%lu,bContainerIndex=%d,dwKeySpec=%lx,pbData=%p,cbData=%d,pPaddingInfo=%p,dwPaddingType=%lu)\n", 
		    pInfo->dwVersion, pInfo->bContainerIndex, pInfo->dwKeySpec, pInfo->pbData, pInfo->cbData, pInfo->pPaddingInfo, pInfo->dwPaddingType);
#endif

	if ((pInfo->dwVersion != CARD_SIGNING_INFO_BASIC_VERSION) && (pInfo->dwVersion != CARD_SIGNING_INFO_CURRENT_VERSION))
		FUNC_FAILS(ERROR_REVISION_MISMATCH, "Version check failed");		// CMR_415

	if (pInfo->pbData == NULL)			// CMR_418
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pInfo->pbData validation failed");

	if (pInfo->dwKeySpec != AT_KEYEXCHANGE)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pInfo->dwKeySpec validation failed");		// CMR_417

	if ((pInfo->cbData < 128) || (pInfo->cbData > sizeof(cryptogram)))
		FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "pInfo->cbData validation failed");

	dwret = validateToken(pCardData, &token);
	if (dwret != SCARD_S_SUCCESS)
		FUNC_FAILS(dwret, "Could not obtain fresh token reference");

	p11prikey = NULL;
	getKeyForIndex(pCardData, (int)pInfo->bContainerIndex, &p11prikey);

	if (p11prikey == NULL)		// CMR_416
		FUNC_FAILS(SCARD_E_NO_KEY_CONTAINER, "bContainerIndex invalid");

	keytype = CKK_RSA;
	if (findAttribute(p11prikey, CKA_KEY_TYPE, &attr)) {
		keytype = *(CK_KEY_TYPE *)attr->attrData.pValue;
	}

	if (keytype != CKK_RSA)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "Key is not a RSA key");

	mech.pParameter = NULL;
	mech.ulParameterLen = 0;

	if (pInfo->dwPaddingType == BCRYPT_PAD_PKCS1) {
		mech.mechanism = CKM_RSA_PKCS;
	} else {
		if (!strncmp((char *)token->info.model, "SmartCard-HSM", 13)) {
			mech.mechanism = CKM_RSA_X_509;
		} else {
			mech.mechanism = CKM_RSA_PKCS_OAEP;
		}
	}

	rc = p11prikey->C_DecryptInit(p11prikey, &mech);

	if (rc != CKR_OK) {
		dwret = mapError(rc);
		FUNC_FAILS(dwret, "C_DecryptInit failed");
	}

	copyInverted(cryptogram, pInfo->pbData, pInfo->cbData);
	plainlen = sizeof(plain);

	rc = p11prikey->C_Decrypt(p11prikey, &mech, cryptogram, pInfo->cbData, plain, &plainlen);

	if (rc != CKR_OK) {
		dwret = mapError(rc);
		FUNC_FAILS(dwret, "C_Decrypt failed");
	}

	copyInverted(pInfo->pbData, plain, plainlen);
	pInfo->cbData = plainlen;

	pp = NULL;
	dwlen = 0;
	if (!strncmp((char *)token->info.model, "SmartCard-HSM", 13) && (pInfo->dwPaddingType != BCRYPT_PAD_PKCS1)) {
		dwret = pCardData->pfnCspUnpadData(pInfo, &dwlen, &pp);

		if (dwret != 0) {
			FUNC_FAILS(dwret, "pfnCspUnpadData() failed");
		}

		memcpy(pInfo->pbData, pp, dwlen);
		pInfo->cbData = dwlen;

		pCardData->pfnCspFree(pp);
	}

	memset(plain, 0xA5, sizeof(plain));

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardSignData(__in PCARD_DATA pCardData, __inout PCARD_SIGNING_INFO pInfo)
{
	struct p11Token_t *token;
	struct p11Object_t *p11prikey;
	struct p11Attribute_t *attr;
	unsigned char signature[512], signInput[90],*di;		// di_sha512 needs 83 bytes
	CK_MECHANISM mech;
	CK_KEY_TYPE keytype;
	CK_ULONG cklen;
	DWORD dwret;
	int rc,dilen;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,pInfo=%p)\n", pCardData, pInfo);
#endif

	if (pCardData == NULL)		// CMR_467
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (pInfo == NULL)			// CMR_468
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pInfo validation failed");

#ifdef DEBUG
	debug(" pInfo(dwVersion=%lu,bContainerIndex=%d,dwKeySpec=%lx,dwSigningFlags=%lx,aiHashAlg=%lx,pbData=%p,cbData=%d,pbSignedData=%p,cbSignedData=%d,pPaddingInfo=%p,dwPaddingType=%lu)\n", 
		    pInfo->dwVersion, pInfo->bContainerIndex, pInfo->dwKeySpec, pInfo->dwSigningFlags, pInfo->aiHashAlg, pInfo->pbData, pInfo->cbData, pInfo->pbSignedData, pInfo->cbSignedData, pInfo->pPaddingInfo, pInfo->dwPaddingType);
#endif

	if ((pInfo->dwVersion != CARD_SIGNING_INFO_BASIC_VERSION) && (pInfo->dwVersion != CARD_SIGNING_INFO_CURRENT_VERSION))
		FUNC_FAILS(ERROR_REVISION_MISMATCH, "Version check failed");		// CMR_469

	if (pInfo->pbData == NULL)			// CMR_470
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pInfo->pbData validation failed");

	if ((pInfo->dwKeySpec != AT_ECDHE_P256) && (pInfo->dwKeySpec != AT_ECDHE_P384) && (pInfo->dwKeySpec != AT_ECDHE_P521) && 
		(pInfo->dwKeySpec != AT_ECDSA_P256) && (pInfo->dwKeySpec != AT_ECDSA_P384) && (pInfo->dwKeySpec != AT_ECDSA_P521) && 
		(pInfo->dwKeySpec != AT_SIGNATURE) && (pInfo->dwKeySpec != AT_KEYEXCHANGE))
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pInfo->dwKeySpec validation failed");		// CMR_472

	// #define CRYPT_NOHASHOID         0x00000001
	// maps to
	// #define CARD_PADDING_NONE       0x00000001
	if (pInfo->dwSigningFlags & ~(CARD_PADDING_INFO_PRESENT|CARD_BUFFER_SIZE_ONLY|CARD_PADDING_NONE|CARD_PADDING_PKCS1|CARD_PADDING_PSS|CARD_PADDING_OAEP))
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pInfo->dwSigningFlags validation failed");	// CMR_

	dwret = validateToken(pCardData, &token);
	if (dwret != SCARD_S_SUCCESS)
		FUNC_FAILS(dwret, "Could not obtain fresh token reference");

	p11prikey = NULL;
	getKeyForIndex(pCardData, (int)pInfo->bContainerIndex, &p11prikey);

	if (p11prikey == NULL)		// CMR_471
		FUNC_FAILS(SCARD_E_NO_KEY_CONTAINER, "bContainerIndex invalid");

	mech.pParameter = NULL;
	mech.ulParameterLen = 0;
	mech.mechanism = CKM_RSA_PKCS;

	keytype = CKK_RSA;
	if (findAttribute(p11prikey, CKA_KEY_TYPE, &attr) >= 0) {
		keytype = *(CK_KEY_TYPE *)attr->attrData.pValue;
		if (keytype == CKK_ECDSA)
			mech.mechanism = CKM_ECDSA;
	}

	di = NULL;
	dilen = 0;
	if (!(pInfo->dwSigningFlags & CARD_PADDING_INFO_PRESENT)) {
		switch(pInfo->aiHashAlg) {
		case 0:
			break;
		case CALG_SHA:
			di = di_sha1;
			dilen = sizeof(di_sha1);
			break;
		case CALG_SHA_256:
			di = di_sha256;
			dilen = sizeof(di_sha256);
			break;
		case CALG_SHA_384:
			di = di_sha384;
			dilen = sizeof(di_sha384);
			break;
		case CALG_SHA_512:
			di = di_sha512;
			dilen = sizeof(di_sha512);
			break;
		case CALG_MD5:
			di = di_md5;
			dilen = sizeof(di_md5);
			break;
		case CALG_SSL3_SHAMD5:
			di = NULL;
			dilen = 0;
			break;
		default:
			FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "aiHashAlg not supported");
		}
	} else {
		if (pInfo->dwPaddingType == CARD_PADDING_PKCS1) {
			BCRYPT_PKCS1_PADDING_INFO *padinfo = (BCRYPT_PKCS1_PADDING_INFO *)pInfo->pPaddingInfo;

			if (!padinfo->pszAlgId)	{		// CALG_SSL3_SHAMD5
				dilen = 0;
			} else if (!wcscmp(padinfo->pszAlgId, BCRYPT_SHA1_ALGORITHM)) {
				di = di_sha1;
				dilen = sizeof(di_sha1);
			} else if (!wcscmp(padinfo->pszAlgId, BCRYPT_SHA256_ALGORITHM)) {
				di = di_sha256;
				dilen = sizeof(di_sha256);
			} else if (!wcscmp(padinfo->pszAlgId, BCRYPT_SHA384_ALGORITHM)) {
				di = di_sha384;
				dilen = sizeof(di_sha384);
			} else if (!wcscmp(padinfo->pszAlgId, BCRYPT_SHA512_ALGORITHM)) {
				di = di_sha512;
				dilen = sizeof(di_sha512);
			} else if (!wcscmp(padinfo->pszAlgId, BCRYPT_MD5_ALGORITHM)) {
				di = di_md5;
				dilen = sizeof(di_md5);
			} else {
				FUNC_FAILS(SCARD_E_UNSUPPORTED_FEATURE, "pszAlgId not supported");
			}
		} else if (pInfo->dwPaddingType == CARD_PADDING_PSS) {
			BCRYPT_PSS_PADDING_INFO *padinfo = (BCRYPT_PSS_PADDING_INFO *)pInfo->pPaddingInfo;

			if (!wcscmp(padinfo->pszAlgId, BCRYPT_SHA1_ALGORITHM)) {
				mech.mechanism = CKM_SC_HSM_PSS_SHA1;
			} else if (!wcscmp(padinfo->pszAlgId, BCRYPT_SHA256_ALGORITHM)) {
				mech.mechanism = CKM_SC_HSM_PSS_SHA256;
			} else if (!wcscmp(padinfo->pszAlgId, BCRYPT_SHA384_ALGORITHM)) {
				mech.mechanism = CKM_SC_HSM_PSS_SHA384;
			} else if (!wcscmp(padinfo->pszAlgId, BCRYPT_SHA512_ALGORITHM)) {
				mech.mechanism = CKM_SC_HSM_PSS_SHA512;
			} else {
				FUNC_FAILS(SCARD_E_UNSUPPORTED_FEATURE, "pszAlgId not supported");
			}
		} else if (pInfo->dwPaddingType != CARD_PADDING_NONE) {
			FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pInfo->dwPaddingType invalid");
		}
	}

	if (dilen > 0)
		memcpy(signInput, di, dilen);

	if (dilen + pInfo->cbData > sizeof(signInput))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Buffer for signature input too small");
		
	memcpy(signInput + dilen, pInfo->pbData, pInfo->cbData);
	dilen += pInfo->cbData;

	rc = p11prikey->C_SignInit(p11prikey, &mech);

	if (rc != CKR_OK) {
		dwret = mapError(rc);
		FUNC_FAILS(dwret, "C_SignInit failed");
	}

	if (pInfo->dwSigningFlags & CARD_BUFFER_SIZE_ONLY) {
		cklen = 0;
		rc = p11prikey->C_Sign(p11prikey, &mech, signInput, dilen, NULL, &cklen);
		pInfo->cbSignedData = cklen;
		pInfo->pbSignedData = NULL;
		FUNC_RETURNS(SCARD_S_SUCCESS);
	}

	cklen = sizeof(signature);
	rc = p11prikey->C_Sign(p11prikey, &mech, signInput, dilen, signature, &cklen);

	if (rc != CKR_OK) {
		dwret = mapError(rc);
		FUNC_FAILS(dwret, "C_SignInit failed");
	}

	pInfo->cbSignedData = cklen;
	pInfo->pbSignedData = (PBYTE)pCardData->pfnCspAlloc(cklen);

	if (pInfo->pbSignedData == NULL)
		FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");

	if (keytype == CKK_RSA)
		copyInverted(pInfo->pbSignedData, signature, cklen);
	else
		memcpy(pInfo->pbSignedData, signature, cklen);
	
	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardQueryKeySizes(__in PCARD_DATA pCardData,
	__in  DWORD dwKeySpec,
	__in  DWORD dwFlags,
	__inout PCARD_KEY_SIZES pKeySizes)
{
	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,dwKeySpec=%lu,dwFlags=%lu,pKeySizes=%p)\n", pCardData, dwKeySpec, dwFlags, pKeySizes);
#endif

	if (pCardData == NULL)		// CMR_482
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (pKeySizes == NULL)		// CMR_483
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pKeySizes validation failed");

	if ((dwKeySpec != 0) && (dwKeySpec != AT_SIGNATURE) && (dwKeySpec != AT_KEYEXCHANGE) &&
		(dwKeySpec != AT_ECDHE_P256) && (dwKeySpec != AT_ECDHE_P384) && (dwKeySpec != AT_ECDHE_P521) &&
		(dwKeySpec != AT_ECDSA_P256) && (dwKeySpec != AT_ECDSA_P384) && (dwKeySpec != AT_ECDSA_P521))
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwKeySpec validation failed");

	if (dwFlags != 0)		// CMR_485
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

	if (pKeySizes->dwVersion > CARD_KEY_SIZES_CURRENT_VERSION)		// CMR_486
		FUNC_FAILS(ERROR_REVISION_MISMATCH, "Structure version mismatch");

	pKeySizes->dwVersion = CARD_KEY_SIZES_CURRENT_VERSION;
	switch(dwKeySpec) {
	case 0:
	case AT_KEYEXCHANGE:
	case AT_SIGNATURE:
		pKeySizes->dwMinimumBitlen = 1024;
		pKeySizes->dwMaximumBitlen = 4096;
		pKeySizes->dwDefaultBitlen = 2048;
		pKeySizes->dwIncrementalBitlen = 8;
		break;

	case AT_ECDSA_P256:
	case AT_ECDHE_P256:
		pKeySizes->dwMinimumBitlen = 256;
		pKeySizes->dwMaximumBitlen = 256;
		pKeySizes->dwDefaultBitlen = 256;
		pKeySizes->dwIncrementalBitlen = 0;
		break;

	default:
		FUNC_FAILS(SCARD_E_UNSUPPORTED_FEATURE, "dwKeySpec contains unknown algorithm");		// CMR_487
	}

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



static DWORD WINAPI CardGetContainerProperty(__in PCARD_DATA pCardData,
	__in BYTE bContainerIndex,
	__in LPCWSTR wszProperty,
	__out_bcount_part_opt(cbData, *pdwDataLen) PBYTE pbData,
	__in DWORD cbData,
	__out PDWORD pdwDataLen,
	__in DWORD dwFlags)
{
	struct p11Object_t *p11prikey;
	DWORD dwret;

	FUNC_CALLED();

	if (pCardData == NULL)		// CMR_389
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

#ifdef DEBUG
	debug(" (pCardData=%p,bContainerIndex=%d,wszProperty='%S',pbData=%p,cbData=%lu,pdwDataLen=%p,dwFlags=%lu )\n", pCardData, bContainerIndex, wszProperty, pbData, cbData, pdwDataLen, dwFlags);
#endif

	if (wszProperty == NULL)	// CMR_391
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "wszProperty validation failed");

	if (pbData == NULL)			// CMR_392
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pbData validation failed");

	if (dwFlags != 0)			// CMR_393
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

	if (pdwDataLen == NULL)		// CMR_328
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pdwDataLen validation failed");

	dwret = SCARD_S_SUCCESS;

	if (wcscmp(CCP_CONTAINER_INFO, wszProperty) == 0) {
		*pdwDataLen = sizeof(CONTAINER_INFO);
		if (cbData < sizeof(CONTAINER_INFO))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CONTAINER_INFO");

		dwret = CardGetContainerInfo(pCardData, bContainerIndex, dwFlags, (PCONTAINER_INFO)pbData);
	
	} else if (wcscmp(CCP_PIN_IDENTIFIER, wszProperty) == 0) {
		p11prikey = NULL;
		getKeyForIndex(pCardData, (int)bContainerIndex, &p11prikey);

		if (p11prikey == NULL) // CMR_390
			FUNC_FAILS(SCARD_E_NO_KEY_CONTAINER, "bContainerIndex invalid");

		*pdwDataLen = sizeof(PIN_ID);
		if (cbData < sizeof(PIN_ID))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for PIN_ID");
		*(PPIN_ID)pbData = ROLE_USER;

	} else { // CMR_391
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "Property unknown");
	}

	FUNC_RETURNS(dwret);
}



static DWORD WINAPI CardGetProperty(__in PCARD_DATA pCardData,
	__in LPCWSTR wszProperty,
	__out_bcount_part_opt(cbData, *pdwDataLen) PBYTE pbData,
	__in DWORD cbData,
	__out PDWORD pdwDataLen,
	__in DWORD dwFlags)
{
	struct p11Slot_t *slot;

	DWORD dwret, i;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,wszProperty='%S',pbData=%p,cbData=%lu,pdwDataLen=%p,dwFlags=%lu )\n", pCardData, wszProperty, pbData, cbData, pdwDataLen, dwFlags);
#endif

	if (pCardData == NULL)		// CMR_324
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (wszProperty == NULL)	// CMR_325
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "wszProperty validation failed");

	if (pbData == NULL)			// CMR_327
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pbData validation failed");

	if (pdwDataLen == NULL)		// CMR_328
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pdwDataLen validation failed");

	if (wcscmp(CP_CARD_KEYSIZES, wszProperty) && 
		wcscmp(CP_CARD_PIN_INFO, wszProperty) && 
		wcscmp(CP_CARD_PIN_STRENGTH_VERIFY, wszProperty) && 
		wcscmp(CP_CARD_PIN_STRENGTH_CHANGE, wszProperty) && 
		wcscmp(CP_CARD_PIN_STRENGTH_UNBLOCK, wszProperty) && 
		(dwFlags != 0))
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");		// CMR_329

	slot = (struct p11Slot_t *)pCardData->pvVendorSpecific;
	dwret = SCARD_S_SUCCESS;

	if (wcscmp(CP_CARD_FREE_SPACE, wszProperty) == 0) {
		*pdwDataLen = sizeof(CARD_FREE_SPACE_INFO);
		if (cbData < sizeof(CARD_FREE_SPACE_INFO))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CARD_FREE_SPACE_INFO");

		dwret = CardQueryFreeSpace(pCardData, dwFlags, (PCARD_FREE_SPACE_INFO)pbData);

	} else if (wcscmp(CP_CARD_CAPABILITIES, wszProperty) == 0) {
		*pdwDataLen = sizeof(CARD_CAPABILITIES);
		if (cbData < sizeof(CARD_CAPABILITIES))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CARD_CAPABILITIES");

		dwret = CardQueryCapabilities(pCardData, (PCARD_CAPABILITIES)pbData);

	} else if (wcscmp(CP_CARD_KEYSIZES, wszProperty) == 0) {
		*pdwDataLen = sizeof(CARD_KEY_SIZES);
		if (cbData < sizeof(CARD_KEY_SIZES))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CARD_KEY_SIZES");

		dwret = CardQueryKeySizes(pCardData, dwFlags, 0, (PCARD_KEY_SIZES)pbData);

	} else if (wcscmp(CP_CARD_READ_ONLY, wszProperty) == 0) {
		*pdwDataLen = sizeof(BOOL);
		if (cbData < sizeof(BOOL))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CP_CARD_READ_ONLY");

		*(PBOOL)pbData = TRUE;

	} else if (wcscmp(CP_CARD_CACHE_MODE, wszProperty) == 0) {
		*pdwDataLen = sizeof(DWORD);
		if (cbData < sizeof(DWORD))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CP_CARD_CACHE_MODE");

		*(PDWORD)pbData = CP_CACHE_MODE_NO_CACHE;

	} else if (wcscmp(CP_SUPPORTS_WIN_X509_ENROLLMENT, wszProperty) == 0) {
		*pdwDataLen = sizeof(BOOL);
		if (cbData < sizeof(BOOL))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CP_SUPPORTS_WIN_X509_ENROLLMENT");

		*(PBOOL)pbData = FALSE;

	} else if (wcscmp(CP_CARD_GUID, wszProperty) == 0) {
		*pdwDataLen = 16;
		if (cbData < 16)
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CP_CARD_GUID");

		CopyMemory(pbData, slot->token->info.serialNumber, 16);

	} else if (wcscmp(CP_CARD_SERIAL_NO, wszProperty) == 0) {
		i = sizeof(slot->token->info.serialNumber);			// Strip trailing blanks
		while ((i > 0) && (slot->token->info.serialNumber[i - 1] == ' ')) i--;

		*pdwDataLen = i;
		if (cbData < i)
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CP_CARD_SERIAL_NO");

		CopyMemory(pbData, slot->token->info.serialNumber, i);

	} else if (wcscmp(CP_CARD_PIN_INFO, wszProperty) == 0) {
		*pdwDataLen = sizeof(PIN_INFO);
		if (cbData < sizeof(PIN_INFO))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for PIN_INFO");

		dwret = CardQueryPINInfo(pCardData, dwFlags, (PPIN_INFO)pbData);

	} else if (wcscmp(CP_CARD_LIST_PINS, wszProperty) == 0) {
		*pdwDataLen = sizeof(PIN_SET);
		if (cbData < sizeof(PIN_SET))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CP_CARD_LIST_PINS");

		*(PPIN_SET)pbData = CREATE_PIN_SET(ROLE_USER);

	} else if (wcscmp(CP_CARD_AUTHENTICATED_STATE, wszProperty) == 0) {
		*pdwDataLen = sizeof(PIN_SET);
		if (cbData < sizeof(PIN_SET))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CP_CARD_AUTHENTICATED_STATE");

		*(PPIN_SET)pbData = 0;
		if (slot->token->user == CKU_USER) {
			*(PPIN_SET)pbData = CREATE_PIN_SET(ROLE_USER);
		}

	} else if (wcscmp(CP_CARD_PIN_STRENGTH_VERIFY, wszProperty) == 0) {
		if (dwFlags != 1)
			FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

		*pdwDataLen = sizeof(DWORD);
		if (cbData < sizeof(DWORD))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CP_CARD_PIN_STRENGTH_VERIFY");

		*(PDWORD)pbData = CARD_PIN_STRENGTH_PLAINTEXT;

	} else if (wcscmp(CP_CARD_PIN_STRENGTH_CHANGE, wszProperty) == 0) {
		if (dwFlags != 1)
			FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

		*pdwDataLen = sizeof(DWORD);
		if (cbData < sizeof(DWORD))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CP_CARD_PIN_STRENGTH_VERIFY");

		FUNC_FAILS(SCARD_E_UNSUPPORTED_FEATURE, "Not supported");

	} else if (wcscmp(CP_CARD_PIN_STRENGTH_UNBLOCK, wszProperty) == 0) {
		if (dwFlags != 1)
			FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

		*pdwDataLen = sizeof(DWORD);
		if (cbData < sizeof(DWORD))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CP_CARD_PIN_STRENGTH_VERIFY");

		FUNC_FAILS(SCARD_E_UNSUPPORTED_FEATURE, "Not supported");

	} else if (wcscmp(CP_KEY_IMPORT_SUPPORT, wszProperty) == 0) {
		*pdwDataLen = sizeof(DWORD);
		if (cbData < sizeof(DWORD))
			FUNC_FAILS(SCARD_E_INSUFFICIENT_BUFFER, "Provided buffer too small for CP_KEY_IMPORT_SUPPORT");

		*(PDWORD)pbData = 0;

	} else {
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "Property unknown");
	}

	FUNC_RETURNS(dwret);
}



static DWORD WINAPI CardSetProperty(__in   PCARD_DATA pCardData,
	__in LPCWSTR wszProperty,
	__in_bcount(cbDataLen)  PBYTE pbData,
	__in DWORD cbDataLen,
	__in DWORD dwFlags)
{
	HWND hnd;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p,wszProperty='%S',pbData=%p,cbDataLen=%lu,dwFlags=%lu )\n", pCardData, wszProperty, pbData, cbDataLen, dwFlags);
#endif

	if (pCardData == NULL)		// CMR_332
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	if (wszProperty == NULL)	// CMR_333
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "wszProperty validation failed");

	if ((wcscmp(CP_PIN_CONTEXT_STRING, wszProperty) != 0) && (pbData == NULL))			// CMR_334
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pbData validation failed");

	if (wcscmp(CP_PARENT_WINDOW, wszProperty) == 0) {
		if (dwFlags != 0)			// CMR_337
			FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");
		if (cbDataLen != sizeof(hnd)) 
			FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "CP_PARENT_WINDOW cbDataLen failed");

		hnd = *(HWND *)pbData;

		if ((hnd != NULL) && !IsWindow(hnd))
			FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "CP_PARENT_WINDOW is not a valid handle");

	} else if (wcscmp(CP_PIN_CONTEXT_STRING, wszProperty) == 0) {
		if (dwFlags != 0)			// CMR_337
			FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "dwFlags validation failed");

	} else {
		FUNC_FAILS(SCARD_E_UNSUPPORTED_FEATURE, "Unsupported wszProperty");
	}

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



DWORD WINAPI CardDeleteContainer(__in PCARD_DATA pCardData,
	__in BYTE bContainerIndex,
	__in DWORD dwReserved)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardCreateContainer(__in PCARD_DATA pCardData,
	__in BYTE bContainerIndex,
	__in DWORD dwFlags,
	__in DWORD dwKeySpec,
	__in DWORD dwKeySize,
	__in PBYTE pbKeyData)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardGetChallenge(__in PCARD_DATA pCardData,
	__deref_out_bcount(*pcbChallengeData) PBYTE *ppbChallengeData,
	__out                                 PDWORD pcbChallengeData)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardAuthenticateChallenge(__in PCARD_DATA  pCardData,
	__in_bcount(cbResponseData) PBYTE  pbResponseData,
	__in DWORD  cbResponseData,
	__out_opt PDWORD pcAttemptsRemaining)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardUnblockPin(__in PCARD_DATA  pCardData,
	__in LPWSTR pwszUserId,
	__in_bcount(cbAuthenticationData) PBYTE  pbAuthenticationData,
	__in DWORD  cbAuthenticationData,
	__in_bcount(cbNewPinData) PBYTE  pbNewPinData,
	__in DWORD  cbNewPinData,
	__in DWORD  cRetryCount,
	__in DWORD  dwFlags)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardChangeAuthenticator(__in PCARD_DATA  pCardData,
	__in LPWSTR pwszUserId,
	__in_bcount(cbCurrentAuthenticator) PBYTE pbCurrentAuthenticator,
	__in DWORD cbCurrentAuthenticator,
	__in_bcount(cbNewAuthenticator) PBYTE pbNewAuthenticator,
	__in DWORD cbNewAuthenticator,
	__in DWORD cRetryCount,
	__in DWORD dwFlags,
	__out_opt PDWORD pcAttemptsRemaining)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardCreateDirectory(__in PCARD_DATA pCardData,
	__in LPSTR pszDirectoryName,
	__in CARD_DIRECTORY_ACCESS_CONDITION AccessCondition)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardDeleteDirectory(__in PCARD_DATA pCardData,
	__in LPSTR pszDirectoryName)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardCreateFile(__in PCARD_DATA pCardData,
	__in_opt LPSTR pszDirectoryName,
	__in LPSTR pszFileName,
	__in DWORD cbInitialCreationSize,
	__in CARD_FILE_ACCESS_CONDITION AccessCondition)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardWriteFile(__in PCARD_DATA pCardData,
	__in_opt LPSTR pszDirectoryName,
	__in LPSTR pszFileName,
	__in DWORD dwFlags,
	__in_bcount(cbData) PBYTE pbData,
	__in DWORD cbData)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardDeleteFile(__in PCARD_DATA pCardData,
	__in_opt LPSTR pszDirectoryName,
	__in LPSTR pszFileName,
	__in DWORD dwFlags)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}


	
DWORD WINAPI CardGetChallengeEx(__in PCARD_DATA pCardData,
	__in PIN_ID PinId,
	__deref_out_bcount(*pcbChallengeData) PBYTE *ppbChallengeData,
	__out PDWORD pcbChallengeData,
	__in DWORD dwFlags)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardChangeAuthenticatorEx(__in PCARD_DATA pCardData,
	__in   DWORD dwFlags,
	__in   PIN_ID dwAuthenticatingPinId,
	__in_bcount(cbAuthenticatingPinData) PBYTE pbAuthenticatingPinData,
	__in   DWORD cbAuthenticatingPinData,
	__in   PIN_ID dwTargetPinId,
	__in_bcount(cbTargetData) PBYTE pbTargetData,
	__in   DWORD cbTargetData,
	__in   DWORD cRetryCount,
	__out_opt PDWORD pcAttemptsRemaining)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardSetContainerProperty(__in PCARD_DATA pCardData,
	__in BYTE bContainerIndex,
	__in LPCWSTR wszProperty,
	__in_bcount(cbDataLen) PBYTE pbData,
	__in DWORD cbDataLen,
	__in DWORD dwFlags)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI MDImportSessionKey(
    __in PCARD_DATA  pCardData,
    __in LPCWSTR  pwszBlobType,
    __in LPCWSTR  pwszAlgId,
    __out PCARD_KEY_HANDLE  phKey,
    __in_bcount(cbInput) PBYTE  pbInput,
    __in DWORD  cbInput)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI MDEncryptData(
    __in PCARD_DATA  pCardData,
    __in CARD_KEY_HANDLE  hKey,
    __in LPCWSTR  pwszSecureFunction,
    __in_bcount(cbInput) PBYTE  pbInput,
    __in DWORD  cbInput,
    __in DWORD  dwFlags,
    __deref_out_ecount(*pcEncryptedData)
        PCARD_ENCRYPTED_DATA  *ppEncryptedData,
    __out PDWORD  pcEncryptedData)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardImportSessionKey(
    __in PCARD_DATA  pCardData,
    __in BYTE  bContainerIndex,
    __in VOID  *pPaddingInfo,
    __in LPCWSTR  pwszBlobType,
    __in LPCWSTR  pwszAlgId,
    __out CARD_KEY_HANDLE  *phKey,
    __in_bcount(cbInput) PBYTE  pbInput,
    __in DWORD  cbInput,
    __in DWORD  dwFlags)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardGetSharedKeyHandle(
    __in PCARD_DATA  pCardData,
    __in_bcount(cbInput) PBYTE  pbInput,
    __in DWORD  cbInput,
    __deref_opt_out_bcount(*pcbOutput)
        PBYTE  *ppbOutput,
    __out_opt PDWORD  pcbOutput,
    __out PCARD_KEY_HANDLE  phKey)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardGetAlgorithmProperty (
    __in PCARD_DATA  pCardData,
    __in LPCWSTR   pwszAlgId,
    __in LPCWSTR   pwszProperty,
    __out_bcount_part_opt(cbData, *pdwDataLen)
        PBYTE  pbData,
    __in DWORD  cbData,
    __out PDWORD  pdwDataLen,
    __in DWORD  dwFlags)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardGetKeyProperty(
    __in PCARD_DATA  pCardData,
    __in CARD_KEY_HANDLE  hKey,
    __in LPCWSTR  pwszProperty,
    __out_bcount_part_opt(cbData, *pdwDataLen) PBYTE  pbData,
    __in DWORD  cbData,
    __out PDWORD  pdwDataLen,
    __in DWORD  dwFlags)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardSetKeyProperty(
    __in PCARD_DATA  pCardData,
    __in CARD_KEY_HANDLE  hKey,
    __in LPCWSTR  pwszProperty,
    __in_bcount(cbInput) PBYTE  pbInput,
    __in DWORD  cbInput,
    __in DWORD  dwFlags)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardDestroyKey(
    __in PCARD_DATA  pCardData,
    __in CARD_KEY_HANDLE  hKey)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardProcessEncryptedData(
    __in PCARD_DATA  pCardData,
    __in CARD_KEY_HANDLE  hKey,
    __in LPCWSTR  pwszSecureFunction,
    __in_ecount(cEncryptedData)
        PCARD_ENCRYPTED_DATA  pEncryptedData,
    __in DWORD  cEncryptedData,
    __out_bcount_part_opt(cbOutput, *pdwOutputLen)
        PBYTE  pbOutput,
    __in DWORD  cbOutput,
    __out_opt PDWORD  pdwOutputLen,
    __in DWORD  dwFlags)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardCreateContainerEx(
    __in PCARD_DATA  pCardData,
    __in BYTE  bContainerIndex,
    __in DWORD  dwFlags,
    __in DWORD  dwKeySpec,
    __in DWORD  dwKeySize,
    __in PBYTE  pbKeyData,
    __in PIN_ID  PinId)
{
	FUNC_CALLED();

	if (pCardData == NULL)
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	FUNC_RETURNS(SCARD_E_UNSUPPORTED_FEATURE);
}



DWORD WINAPI CardAcquireContext(__inout PCARD_DATA pCardData, __in DWORD dwFlags)
{
	struct p11Slot_t *slot;
	struct p11Token_t *token;
	DWORD version,dwret;
	CHAR reader[200];
	DWORD readerlen = sizeof(reader);
	BYTE bAttr[32];
	DWORD cByte = sizeof(bAttr);
	DWORD dwState, dwProtocol;

	int rc;

	FUNC_CALLED();

#ifdef DEBUG
	debug(" (pCardData=%p)\n", pCardData);
#endif

	if (pCardData == NULL)		// CMR_35
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

#ifdef DEBUG
	debug("  pCardData(dwVersion=%lu,hSCardCtx=%lx,hScard=%lx,pwszCardName='%S')\n", pCardData->dwVersion, pCardData->hSCardCtx, pCardData->hScard, pCardData->pwszCardName);
#endif


	if (dwFlags != 0)			// CMR_36
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData validation failed");

	version = pCardData->dwVersion;

	if (pCardData->dwVersion > MAXIMUM_SUPPORTED_VERSION) {
		pCardData->dwVersion = MAXIMUM_SUPPORTED_VERSION;
	}

	if (version < MINIMUM_SUPPORTED_VERSION) {	// CMR_37
		FUNC_FAILS(ERROR_REVISION_MISMATCH, "Requested version lower than minimum version");
	}

	if (pCardData->pbAtr == NULL)		// CMR_38
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData->pbAtr validation failed");

	if ((pCardData->cbAtr < 4) || (pCardData->cbAtr > 33))		// CMR_39
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData->cbAtr validation failed");

	if (pCardData->pbAtr[0] != 0x3B)		// CMR_41
		FUNC_FAILS(SCARD_E_UNKNOWN_CARD, "pCardData->pbAtr validation failed");

	if (pCardData->pwszCardName == NULL)		// CMR_40
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData->pwszCardName validation failed");

	// ToDo: CMR_41

	if ((pCardData->pfnCspAlloc == NULL) || (pCardData->pfnCspReAlloc == NULL) || (pCardData->pfnCspFree == NULL))		// CMR_42
		FUNC_FAILS(SCARD_E_INVALID_PARAMETER, "pCardData->pfnCspAlloc validation failed");

	// ToDo: CMR_43

	if (pCardData->hScard == 0)		// CMR_44
		FUNC_FAILS(SCARD_E_INVALID_HANDLE, "pCardData->hScard validation failed");

	if (pCardData->dwVersion > MAXIMUM_SUPPORTED_VERSION)
		pCardData->dwVersion = MAXIMUM_SUPPORTED_VERSION;

	// Supported calls
    pCardData->pfnCardDeleteContext = CardDeleteContext;
    pCardData->pfnCardQueryCapabilities = CardQueryCapabilities;
    pCardData->pfnCardGetContainerInfo = CardGetContainerInfo;
    pCardData->pfnCardAuthenticatePin = CardAuthenticatePin;
    pCardData->pfnCardDeauthenticate = CardDeauthenticate;
    pCardData->pvUnused3 = NULL;
    pCardData->pvUnused4 = NULL;
    pCardData->pfnCardReadFile = CardReadFile;
    pCardData->pfnCardEnumFiles = CardEnumFiles;
    pCardData->pfnCardGetFileInfo = CardGetFileInfo;
    pCardData->pfnCardQueryFreeSpace = CardQueryFreeSpace;
    pCardData->pfnCardQueryKeySizes = CardQueryKeySizes;

    pCardData->pfnCardSignData = CardSignData;
    pCardData->pfnCardRSADecrypt = CardRSADecrypt;

	// Unsupported calls
    pCardData->pfnCardDeleteContainer = CardDeleteContainer;
    pCardData->pfnCardCreateContainer = CardCreateContainer;
    pCardData->pfnCardGetChallenge = CardGetChallenge;
    pCardData->pfnCardAuthenticateChallenge = CardAuthenticateChallenge;
    pCardData->pfnCardUnblockPin = CardUnblockPin;
    pCardData->pfnCardChangeAuthenticator = CardChangeAuthenticator;
    pCardData->pfnCardCreateDirectory = CardCreateDirectory;
    pCardData->pfnCardDeleteDirectory = CardDeleteDirectory;
    pCardData->pfnCardCreateFile = CardCreateFile;
    pCardData->pfnCardWriteFile = CardWriteFile;
    pCardData->pfnCardDeleteFile = CardDeleteFile;

	pCardData->pfnCardConstructDHAgreement = NULL;		// PFN_CARD_CONSTRUCT_DH_AGREEMENT

	if (pCardData->dwVersion >= CARD_DATA_VERSION_FIVE) {
		pCardData->pfnCardDeriveKey = NULL;				// PFN_CARD_DERIVE_KEY
		pCardData->pfnCardDestroyDHAgreement = NULL;	// PFN_CARD_DESTROY_DH_AGREEMENT
	}

	if (pCardData->dwVersion >= CARD_DATA_VERSION_SIX) {
		// Supported calls
		pCardData->pfnCardAuthenticateEx = CardAuthenticateEx;
		pCardData->pfnCardDeauthenticateEx = CardDeauthenticateEx;
		pCardData->pfnCardGetContainerProperty = CardGetContainerProperty;
		pCardData->pfnCardGetProperty = CardGetProperty;
		pCardData->pfnCardSetProperty = CardSetProperty;

		// Unsupported calls
		pCardData->pfnCardGetChallengeEx = CardGetChallengeEx;
		pCardData->pfnCardChangeAuthenticatorEx = CardChangeAuthenticatorEx;
		pCardData->pfnCardSetContainerProperty = CardSetContainerProperty;
	}

	if (pCardData->dwVersion >= CARD_DATA_VERSION_SEVEN) {
		pCardData->pfnMDImportSessionKey = MDImportSessionKey;
		pCardData->pfnMDEncryptData = MDEncryptData;
		pCardData->pfnCardImportSessionKey = CardImportSessionKey;
		pCardData->pfnCardGetSharedKeyHandle = CardGetSharedKeyHandle;
		pCardData->pfnCardGetAlgorithmProperty = CardGetAlgorithmProperty;
		pCardData->pfnCardGetKeyProperty = CardGetKeyProperty;
		pCardData->pfnCardSetKeyProperty = CardSetKeyProperty;
		pCardData->pfnCardDestroyKey = CardDestroyKey;
		pCardData->pfnCardProcessEncryptedData = CardProcessEncryptedData;
		pCardData->pfnCardCreateContainerEx = CardCreateContainerEx;
	}

	slot = pCardData->pvVendorSpecific = pCardData->pfnCspAlloc(sizeof(struct p11Slot_t));
	if (!slot)
		FUNC_FAILS(SCARD_E_NO_MEMORY, "Out of memory");

	memset(slot, 0, sizeof(struct p11Slot_t));

	slot->card = pCardData->hScard;
	slot->context = pCardData->hSCardCtx;
	slot->maxCAPDU = MAX_CAPDU;
	slot->maxRAPDU = MAX_RAPDU;

	if (SCardStatus(pCardData->hScard,
					reader, &readerlen,
					&dwState,
					&dwProtocol,
					(LPBYTE)&bAttr,
					&cByte) == SCARD_S_SUCCESS) {
		if (!strncmp(reader, "Secure Flash Card", 17)) {
			slot->maxCAPDU = 478;
			slot->maxRAPDU = 506;
			slot->noExtLengthReadAll = TRUE;
		}
	}

	checkPCSCPinPad(slot);

	rc = newToken(slot, pCardData->pbAtr, pCardData->cbAtr, &token);

	if (rc == CKR_OK) {
		dwret = SCARD_S_SUCCESS;
		if (strncmp((char *)token->info.model, "SmartCard-HSM", 13)) {
			pCardData->pfnCardDeauthenticate = NULL;
		}
	} else if (rc == CKR_TOKEN_NOT_RECOGNIZED) {
		dwret = SCARD_E_UNKNOWN_CARD;
	} else {
		dwret = SCARD_E_UNEXPECTED;
	}

	if (rc != CKR_OK) {
		pCardData->pfnCspFree(pCardData->pvVendorSpecific);
		pCardData->pvVendorSpecific = NULL;
	}

	FUNC_RETURNS(SCARD_S_SUCCESS);
}



BOOL APIENTRY DllMain( HINSTANCE hinstDLL,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	CHAR name[MAX_PATH + 1] = "\0";

	GetModuleFileNameA(GetModuleHandle(NULL),name,MAX_PATH);

	switch (ul_reason_for_call)   {
	case DLL_PROCESS_ATTACH:
#ifdef DEBUG
		initDebug("minidriver");
		debug("Process %s attached\n", name);
#endif
		break;
	case DLL_THREAD_ATTACH:
#ifdef DEBUG
		debug("Thread in Process %s attached\n", name);
		break;
#endif
	case DLL_THREAD_DETACH:
#ifdef DEBUG
		debug("Thread in Process %s detached\n", name);
#endif
		break;
	case DLL_PROCESS_DETACH:
#ifdef DEBUG
		debug("Process %s detached\n", name);
		termDebug();
#endif
		break;
	}

	return TRUE;
}

