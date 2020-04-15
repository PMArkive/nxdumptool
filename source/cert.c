#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cert.h"
#include "save.h"
#include "utils.h"

#define CERT_SAVEFILE_PATH              "sys:/save/80000000000000e0"
#define CERT_SAVEFILE_STORAGE_BASE_PATH "/certificate/"

#define CERT_TYPE(sig)  (pub_key_type == CertPubKeyType_Rsa4096 ? CertType_Sig##sig##_PubKeyRsa4096 : (pub_key_type == CertPubKeyType_Rsa2048 ? CertType_Sig##sig##_PubKeyRsa2048 : CertType_Sig##sig##_PubKeyEcsda240))

/* Function prototypes. */

static u8 certGetCertificateType(const void *data, u64 data_size);
static u32 certGetCertificateCountInSignatureIssuer(const char *issuer);
static u64 certCalculateRawCertificateChainSize(const CertificateChain *chain);
static void certCopyCertificateChainDataToMemoryBuffer(void *dst, const CertificateChain *chain);

bool certRetrieveCertificateByName(Certificate *dst, const char *name)
{
    if (!dst || !name || !strlen(name))
    {
        LOGFILE("Invalid parameters!");
        return false;
    }
    
    save_ctx_t *save_ctx = NULL;
    allocation_table_storage_ctx_t fat_storage = {0};
    
    u64 cert_size = 0;
    char cert_path[SAVE_FS_LIST_MAX_NAME_LENGTH] = {0};
    
    bool success = false;
    
    snprintf(cert_path, SAVE_FS_LIST_MAX_NAME_LENGTH, "%s%s", CERT_SAVEFILE_STORAGE_BASE_PATH, name);
    
    save_ctx = save_open_savefile(CERT_SAVEFILE_PATH, 0);
    if (!save_ctx)
    {
        LOGFILE("Failed to open ES certificate system savefile!");
        return false;
    }
    
    if (!save_get_fat_storage_from_file_entry_by_path(save_ctx, cert_path, &fat_storage, &cert_size))
    {
        LOGFILE("Failed to locate certificate \"%s\" in ES certificate system save!", name);
        goto out;
    }
    
    if (cert_size < CERT_MIN_SIZE || cert_size > CERT_MAX_SIZE)
    {
        LOGFILE("Invalid size for certificate \"%s\"! (0x%lX)", name, cert_size);
        goto out;
    }
    
    dst->size = cert_size;
    
    u64 br = save_allocation_table_storage_read(&fat_storage, dst->data, 0, dst->size);
    if (br != dst->size)
    {
        LOGFILE("Failed to read 0x%lX bytes from certificate \"%s\"! Read 0x%lX bytes.", dst->size, name, br);
        goto out;
    }
    
    dst->type = certGetCertificateType(dst->data, dst->size);
    if (dst->type == CertType_Invalid)
    {
        LOGFILE("Invalid certificate type for \"%s\"!", name);
        goto out;
    }
    
    success = true;
    
out:
    if (save_ctx) save_close_savefile(save_ctx);
    
    return success;
}

void certFreeCertificateChain(CertificateChain *chain)
{
    if (!chain || !chain->certs) return;
    
    chain->count = 0;
    free(chain->certs);
    chain->certs = NULL;
}

bool certRetrieveCertificateChainBySignatureIssuer(CertificateChain *dst, const char *issuer)
{
    if (!dst || !issuer || !strlen(issuer) || strncmp(issuer, "Root-", 5) != 0)
    {
        LOGFILE("Invalid parameters!");
        return false;
    }
    
    u32 i = 0;
    char issuer_copy[0x40] = {0};
    bool success = true;
    
    dst->count = certGetCertificateCountInSignatureIssuer(issuer);
    if (!dst->count)
    {
        LOGFILE("Invalid signature issuer string!");
        return false;
    }
    
    dst->certs = calloc(dst->count, sizeof(Certificate));
    if (!dst->certs)
    {
        LOGFILE("Unable to allocate memory for the certificate chain! (0x%lX)", dst->count * sizeof(Certificate));
        return false;
    }
    
    /* Copy string to avoid problems with strtok */
    /* The "Root-" parent from the issuer string is skipped */
    snprintf(issuer_copy, 0x40, issuer + 5);
    
    char *pch = strtok(issuer_copy, "-");
    while(pch != NULL)
    {
        if (!certRetrieveCertificateByName(&(dst->certs[i]), pch))
        {
            LOGFILE("Unable to retrieve certificate \"%s\"!", pch);
            success = false;
            break;
        }
        
        i++;
        pch = strtok(NULL, "-");
    }
    
    if (!success) certFreeCertificateChain(dst);
    
    return success;
}

u8 *certGenerateRawCertificateChainBySignatureIssuer(const char *issuer, u64 *out_size)
{
    if (!issuer || !strlen(issuer) || !out_size)
    {
        LOGFILE("Invalid parameters!");
        return NULL;
    }
    
    CertificateChain chain = {0};
    u8 *raw_chain = NULL;
    u64 raw_chain_size = 0;
    
    if (!certRetrieveCertificateChainBySignatureIssuer(&chain, issuer))
    {
        LOGFILE("Error retrieving certificate chain for \"%s\"!", issuer);
        return NULL;
    }
    
    raw_chain_size = certCalculateRawCertificateChainSize(&chain);
    
    raw_chain = malloc(raw_chain_size);
    if (!raw_chain)
    {
        LOGFILE("Unable to allocate memory for raw \"%s\" certificate chain! (0x%lX)", issuer, raw_chain_size);
        goto out;
    }
    
    certCopyCertificateChainDataToMemoryBuffer(raw_chain, &chain);
    *out_size = raw_chain_size;
    
out:
    certFreeCertificateChain(&chain);
    
    return raw_chain;
}

static u8 certGetCertificateType(const void *data, u64 data_size)
{
    if (!data || data_size < CERT_MIN_SIZE || data_size > CERT_MAX_SIZE)
    {
        LOGFILE("Invalid parameters!");
        return CertType_Invalid;
    }
    
    u8 type = CertType_Invalid;
    const u8 *data_u8 = (const u8*)data;
    u32 sig_type, pub_key_type;
    u64 offset = 0;
    
    memcpy(&sig_type, data_u8, sizeof(u32));
    sig_type = __builtin_bswap32(sig_type);
    
    switch(sig_type)
    {
        case SignatureType_Rsa4096Sha1:
        case SignatureType_Rsa4096Sha256:
            offset += sizeof(SignatureBlockRsa4096);
            break;
        case SignatureType_Rsa2048Sha1:
        case SignatureType_Rsa2048Sha256:
            offset += sizeof(SignatureBlockRsa2048);
            break;
        case SignatureType_Ecsda240Sha1:
        case SignatureType_Ecsda240Sha256:
            offset += sizeof(SignatureBlockEcsda240);
            break;
        default:
            LOGFILE("Invalid signature type value! (0x%08X)", sig_type);
            return type;
    }
    
    offset += MEMBER_SIZE(CertSigRsa4096PubKeyRsa4096, issuer);
    
    memcpy(&pub_key_type, data_u8 + offset, sizeof(u32));
    pub_key_type = __builtin_bswap32(pub_key_type);
    
    offset += MEMBER_SIZE(CertSigRsa4096PubKeyRsa4096, pub_key_type);
    offset += MEMBER_SIZE(CertSigRsa4096PubKeyRsa4096, name);
    offset += MEMBER_SIZE(CertSigRsa4096PubKeyRsa4096, cert_id);
    
    switch(pub_key_type)
    {
        case CertPubKeyType_Rsa4096:
            offset += sizeof(CertPublicKeyBlockRsa4096);
            break;
        case CertPubKeyType_Rsa2048:
            offset += sizeof(CertPublicKeyBlockRsa2048);
            break;
        case CertPubKeyType_Ecsda240:
            offset += sizeof(CertPublicKeyBlockEcsda240);
            break;
        default:
            LOGFILE("Invalid public key type value! (0x%08X)", pub_key_type);
            return type;
    }
    
    if (offset != data_size)
    {
        LOGFILE("Calculated end offset doesn't match certificate size! 0x%lX != 0x%lX", offset, data_size);
        return type;
    }
    
    if (sig_type == SignatureType_Rsa4096Sha1 || sig_type == SignatureType_Rsa4096Sha256)
    {
        type = CERT_TYPE(Rsa4096);
    } else
    if (sig_type == SignatureType_Rsa2048Sha1 || sig_type == SignatureType_Rsa2048Sha256)
    {
        type = CERT_TYPE(Rsa2048);
    } else
    if (sig_type == SignatureType_Ecsda240Sha1 || sig_type == SignatureType_Ecsda240Sha256)
    {
        type = CERT_TYPE(Ecsda240);
    }
    
    return type;
}

static u32 certGetCertificateCountInSignatureIssuer(const char *issuer)
{
    if (!issuer || !strlen(issuer)) return 0;
    
    u32 count = 0;
    char issuer_copy[0x40] = {0};
    
    /* Copy string to avoid problems with strtok */
    /* The "Root-" parent from the issuer string is skipped */
    snprintf(issuer_copy, 0x40, issuer + 5);
    
    char *pch = strtok(issuer_copy, "-");
    while(pch != NULL)
    {
        count++;
        pch = strtok(NULL, "-");
    }
    
    return count;
}

static u64 certCalculateRawCertificateChainSize(const CertificateChain *chain)
{
    if (!chain || !chain->count || !chain->certs) return 0;
    
    u64 chain_size = 0;
    for(u32 i = 0; i < chain->count; i++) chain_size += chain->certs[i].size;
    return chain_size;
}

static void certCopyCertificateChainDataToMemoryBuffer(void *dst, const CertificateChain *chain)
{
    if (!chain || !chain->count || !chain->certs) return;
    
    u8 *dst_u8 = (u8*)dst;
    for(u32 i = 0; i < chain->count; i++)
    {
        memcpy(dst_u8, chain->certs[i].data, chain->certs[i].size);
        dst_u8 += chain->certs[i].size;
    }
}