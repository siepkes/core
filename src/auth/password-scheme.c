/* Copyright (C) 2003-2007 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "base64.h"
#include "hex-binary.h"
#include "md4.h"
#include "md5.h"
#include "hmac-md5.h"
#include "ntlm.h"
#include "module-dir.h"
#include "mycrypt.h"
#include "randgen.h"
#include "sha1.h"
#include "otp.h"
#include "str.h"
#include "password-scheme.h"

static const char salt_chars[] =
	"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static ARRAY_DEFINE(schemes_arr, struct password_scheme);
static const struct password_scheme *schemes;
#ifdef HAVE_MODULES
static struct module *scheme_modules;
#endif

/* Lookup scheme and encoding by given name. The encoding is taken from
   ".base64", ".b64" or ".hex" suffix if it exists, otherwise the default
   encoding is used. */
static const struct password_scheme *
password_scheme_lookup(const char *scheme, enum password_encoding *encoding_r)
{
	const struct password_scheme *s;
	const char *encoding = NULL;
	unsigned int scheme_len;

	*encoding_r = PW_ENCODING_NONE;

	for (scheme_len = 0; scheme[scheme_len] != '\0'; scheme_len++) {
		if (scheme[scheme_len] == '.') {
			encoding = scheme + scheme_len + 1;
			break;
		}
	}

	for (s = schemes; s->name != NULL; s++) {
		if (strncasecmp(s->name, scheme, scheme_len) == 0 &&
		    s->name[scheme_len] == '\0') {
			if (encoding == NULL)
				*encoding_r = s->default_encoding;
			else if (strcasecmp(encoding, "b64") == 0 ||
				 strcasecmp(encoding, "base64") == 0)
				*encoding_r = PW_ENCODING_BASE64;
			else if (strcasecmp(encoding, "hex") == 0)
				*encoding_r = PW_ENCODING_HEX;
			else {
				/* unknown encoding. treat as invalid scheme. */
				return NULL;
			}
			return s;
		}
	}
	return NULL;
}

int password_verify(const char *plaintext, const char *user, const char *scheme,
		    const unsigned char *raw_password, size_t size)
{
	const struct password_scheme *s;
	enum password_encoding encoding;
	const unsigned char *generated;
	size_t generated_size;

	s = password_scheme_lookup(scheme, &encoding);
	if (s == NULL)
		return -1;

	if (s->password_verify != NULL)
		return s->password_verify(plaintext, user, raw_password, size);

	/* generic verification handler: generate the password and compare it
	   to the one in database */
	s->password_generate(plaintext, user, &generated, &generated_size);
	return size != generated_size ? 0 :
		memcmp(generated, raw_password, size) == 0;
}

const char *password_list_schemes(const struct password_scheme **listptr)
{
	if (*listptr == NULL)
		*listptr = schemes;

	if ((*listptr)->name == NULL) {
		*listptr = NULL;
		return NULL;
	}

	return (*listptr)++->name;
}

const char *password_get_scheme(const char **password)
{
	const char *p, *scheme;

	if (*password == NULL)
		return NULL;

	if (strncmp(*password, "$1$", 3) == 0) {
		/* $1$<salt>$<password>[$<ignored>] */
		p = strchr(*password + 3, '$');
		if (p != NULL) {
			/* stop at next '$' after password */
			p = strchr(p+1, '$');
			if (p != NULL)
				*password = t_strdup_until(*password, p);
			return "MD5-CRYPT";
		}
	}

	if (**password != '{')
		return NULL;

	p = strchr(*password, '}');
	if (p == NULL)
		return NULL;

	scheme = t_strdup_until(*password + 1, p);
	*password = p + 1;
	return scheme;
}

int password_decode(const char *password, const char *scheme,
		    const unsigned char **raw_password_r, size_t *size_r)
{
	const struct password_scheme *s;
	enum password_encoding encoding;
	buffer_t *buf;
	unsigned int len;

	s = password_scheme_lookup(scheme, &encoding);
	if (s == NULL)
		return 0;

	len = strlen(password);
	if (encoding != PW_ENCODING_NONE && s->raw_password_len != 0 &&
	    strchr(scheme, '.') == NULL) {
		/* encoding not specified. we can autodetect between
		   base64 and hex encodings. */
		encoding = len == s->raw_password_len * 2 ? PW_ENCODING_HEX :
			PW_ENCODING_BASE64;
	}

	switch (encoding) {
	case PW_ENCODING_NONE:
		*raw_password_r = (const unsigned char *)password;
		*size_r = len;
		break;
	case PW_ENCODING_BASE64:
		buf = buffer_create_static_hard(pool_datastack_create(),
						MAX_BASE64_DECODED_SIZE(len));
		if (base64_decode(password, len, NULL, buf) < 0)
			return -1;

		*raw_password_r = buf->data;
		*size_r = buf->used;
		break;
	case PW_ENCODING_HEX:
		buf = buffer_create_static_hard(pool_datastack_create(),
						len / 2 + 1);
		if (hex_to_binary(password, buf) < 0)
			return -1;

		*raw_password_r = buf->data;
		*size_r = buf->used;
		break;
	}
	if (s->raw_password_len != *size_r && s->raw_password_len != 0) {
		/* password has invalid length */
		return -1;
	}
	return 1;
}

bool password_generate(const char *plaintext, const char *user,
		       const char *scheme,
		       const unsigned char **raw_password_r, size_t *size_r)
{
	const struct password_scheme *s;
	enum password_encoding encoding;

	s = password_scheme_lookup(scheme, &encoding);
	if (s == NULL)
		return FALSE;

	s->password_generate(plaintext, user, raw_password_r, size_r);
	return TRUE;
}

bool password_generate_encoded(const char *plaintext, const char *user,
			       const char *scheme, const char **password_r)
{
	const struct password_scheme *s;
	const unsigned char *raw_password;
	enum password_encoding encoding;
	string_t *str;
	size_t size;

	s = password_scheme_lookup(scheme, &encoding);
	if (s == NULL)
		return FALSE;

	s->password_generate(plaintext, user, &raw_password, &size);
	switch (encoding) {
	case PW_ENCODING_NONE:
		*password_r = t_strndup(raw_password, size);
		break;
	case PW_ENCODING_BASE64:
		str = t_str_new(MAX_BASE64_ENCODED_SIZE(size) + 1);
		base64_encode(raw_password, size, str);
		*password_r = str_c(str);
		break;
	case PW_ENCODING_HEX:
		*password_r = binary_to_hex(raw_password, size);
		break;
	}
	return TRUE;
}

bool password_scheme_is_alias(const char *scheme1, const char *scheme2)
{
	const struct password_scheme *s, *s1 = NULL, *s2 = NULL;

	scheme1 = t_strcut(scheme1, '.');
	scheme2 = t_strcut(scheme2, '.');

	if (strcasecmp(scheme1, scheme2) == 0)
		return TRUE;

	for (s = schemes; s->name != NULL; s++) {
		if (strcasecmp(s->name, scheme1) == 0)
			s1 = s;
		else if (strcasecmp(s->name, scheme2) == 0)
			s2 = s;
	}

	/* if they've the same generate function, they're equivalent */
	return s1 != NULL && s2 != NULL &&
		s1->password_generate == s2->password_generate;
}

static bool
crypt_verify(const char *plaintext, const char *user __attr_unused__,
	     const unsigned char *raw_password, size_t size)
{
	const char *password;

	if (size == 0) {
		/* the default mycrypt() handler would return match */
		return FALSE;
	}

	password = t_strndup(raw_password, size);
	return strcmp(mycrypt(plaintext, password), password) == 0;
}

static void
crypt_generate(const char *plaintext, const char *user __attr_unused__,
	       const unsigned char **raw_password_r, size_t *size_r)
{
	char salt[3];
	const char *password;

	random_fill(salt, sizeof(salt)-1);
	salt[0] = salt_chars[salt[0] % (sizeof(salt_chars)-1)];
	salt[1] = salt_chars[salt[1] % (sizeof(salt_chars)-1)];
	salt[2] = '\0';

	password = t_strdup(mycrypt(plaintext, salt));
	*raw_password_r = (const unsigned char *)password;
	*size_r = strlen(password);
}

static bool
md5_crypt_verify(const char *plaintext, const char *user __attr_unused__,
		 const unsigned char *raw_password, size_t size)
{
	const char *password, *str;

	password = t_strndup(raw_password, size);
	str = password_generate_md5_crypt(plaintext, password);
	return strcmp(str, password) == 0;
}

static void
md5_crypt_generate(const char *plaintext, const char *user __attr_unused__,
		   const unsigned char **raw_password_r, size_t *size_r)
{
	const char *password;
	char salt[9];
	unsigned int i;

	random_fill(salt, sizeof(salt)-1);
	for (i = 0; i < sizeof(salt)-1; i++)
		salt[i] = salt_chars[salt[i] % (sizeof(salt_chars)-1)];
	salt[sizeof(salt)-1] = '\0';

	password = password_generate_md5_crypt(plaintext, salt);
	*raw_password_r = (const unsigned char *)password;
	*size_r = strlen(password);
}

static void
sha1_generate(const char *plaintext, const char *user __attr_unused__,
	      const unsigned char **raw_password_r, size_t *size_r)
{
	unsigned char *digest;

	digest = t_malloc(SHA1_RESULTLEN);
	sha1_get_digest(plaintext, strlen(plaintext), digest);

	*raw_password_r = digest;
	*size_r = SHA1_RESULTLEN;
}

static void
ssha_generate(const char *plaintext, const char *user __attr_unused__,
	      const unsigned char **raw_password_r, size_t *size_r)
{
#define SSHA_SALT_LEN 4
	unsigned char *digest, *salt;
	struct sha1_ctxt ctx;

	digest = t_malloc(SHA1_RESULTLEN + SSHA_SALT_LEN);
	salt = digest + SHA1_RESULTLEN;
	random_fill(salt, SSHA_SALT_LEN);

	sha1_init(&ctx);
	sha1_loop(&ctx, plaintext, strlen(plaintext));
	sha1_loop(&ctx, salt, SSHA_SALT_LEN);
	sha1_result(&ctx, digest);

	*raw_password_r = digest;
	*size_r = SHA1_RESULTLEN + SSHA_SALT_LEN;
}

static bool ssha_verify(const char *plaintext, const char *user,
			const unsigned char *raw_password, size_t size)
{
	unsigned char sha1_digest[SHA1_RESULTLEN];
	struct sha1_ctxt ctx;

	/* format: <SHA1 hash><salt> */
	if (size <= SHA1_RESULTLEN) {
		i_error("ssha_verify(%s): SSHA password too short", user);
		return FALSE;
	}

	sha1_init(&ctx);
	sha1_loop(&ctx, plaintext, strlen(plaintext));
	sha1_loop(&ctx, raw_password + SHA1_RESULTLEN, size - SHA1_RESULTLEN);
	sha1_result(&ctx, sha1_digest);
	return memcmp(sha1_digest, raw_password, SHA1_RESULTLEN) == 0;
}

static void
smd5_generate(const char *plaintext, const char *user __attr_unused__,
	      const unsigned char **raw_password_r, size_t *size_r)
{
#define SMD5_SALT_LEN 4
	unsigned char *digest, *salt;
	struct md5_context ctx;

	digest = t_malloc(MD5_RESULTLEN + SMD5_SALT_LEN);
	salt = digest + MD5_RESULTLEN;
	random_fill(salt, SMD5_SALT_LEN);

	md5_init(&ctx);
	md5_update(&ctx, plaintext, strlen(plaintext));
	md5_update(&ctx, salt, SMD5_SALT_LEN);
	md5_final(&ctx, digest);

	*raw_password_r = digest;
	*size_r = MD5_RESULTLEN + SMD5_SALT_LEN;
}

static bool smd5_verify(const char *plaintext, const char *user,
			const unsigned char *raw_password, size_t size)
{
	unsigned char md5_digest[MD5_RESULTLEN];
	struct md5_context ctx;

	/* format: <MD5 hash><salt> */
	if (size <= MD5_RESULTLEN) {
		i_error("smd5_verify(%s): SMD5 password too short", user);
		return FALSE;
	}

	md5_init(&ctx);
	md5_update(&ctx, plaintext, strlen(plaintext));
	md5_update(&ctx, raw_password + MD5_RESULTLEN, size - MD5_RESULTLEN);
	md5_final(&ctx, md5_digest);
	return memcmp(md5_digest, raw_password, MD5_RESULTLEN) == 0;
}

static void
plain_generate(const char *plaintext, const char *user __attr_unused__,
	       const unsigned char **raw_password_r, size_t *size_r)
{
	*raw_password_r = (const unsigned char *)plaintext,
	*size_r = strlen(plaintext);
}

static void
cram_md5_generate(const char *plaintext, const char *user __attr_unused__,
		  const unsigned char **raw_password_r, size_t *size_r)
{
	struct hmac_md5_context ctx;
	unsigned char *context_digest;

	context_digest = t_malloc(CRAM_MD5_CONTEXTLEN);
	hmac_md5_init(&ctx, (const unsigned char *)plaintext,
		      strlen(plaintext));
	hmac_md5_get_cram_context(&ctx, context_digest);

	*raw_password_r = context_digest;
	*size_r = CRAM_MD5_CONTEXTLEN;
}

static void
digest_md5_generate(const char *plaintext, const char *user,
		    const unsigned char **raw_password_r, size_t *size_r)
{
	const char *realm, *str;
	unsigned char *digest;

	if (user == NULL)
		i_panic("digest_md5_generate(): username not given");

	/* user:realm:passwd */
	realm = strchr(user, '@');
	if (realm != NULL) realm++; else realm = "";

	digest = t_malloc(MD5_RESULTLEN);
	str = t_strdup_printf("%s:%s:%s", t_strcut(user, '@'),
			      realm, plaintext);
	md5_get_digest(str, strlen(str), digest);

	*raw_password_r = digest;
	*size_r = MD5_RESULTLEN;
}

static void
plain_md4_generate(const char *plaintext, const char *user __attr_unused__,
		   const unsigned char **raw_password_r, size_t *size_r)
{
	unsigned char *digest;

	digest = t_malloc(MD4_RESULTLEN);
	md4_get_digest(plaintext, strlen(plaintext), digest);

	*raw_password_r = digest;
	*size_r = MD4_RESULTLEN;
}

static void
plain_md5_generate(const char *plaintext, const char *user __attr_unused__,
		   const unsigned char **raw_password_r, size_t *size_r)
{
	unsigned char *digest;

	digest = t_malloc(MD5_RESULTLEN);
	md5_get_digest(plaintext, strlen(plaintext), digest);

	*raw_password_r = digest;
	*size_r = MD5_RESULTLEN;
}

static void
lm_generate(const char *plaintext, const char *user __attr_unused__,
	    const unsigned char **raw_password_r, size_t *size_r)
{
	unsigned char *digest;

	digest = t_malloc(LM_HASH_SIZE);
	lm_hash(plaintext, digest);

	*raw_password_r = digest;
	*size_r = LM_HASH_SIZE;
}

static void
ntlm_generate(const char *plaintext, const char *user __attr_unused__,
	      const unsigned char **raw_password_r, size_t *size_r)
{
	unsigned char *digest;

	digest = t_malloc(NTLMSSP_HASH_SIZE);
	ntlm_v1_hash(plaintext, digest);

	*raw_password_r = digest;
	*size_r = NTLMSSP_HASH_SIZE;
}

static bool otp_verify(const char *plaintext, const char *user __attr_unused__,
		       const unsigned char *raw_password, size_t size)
{
	const char *password;

	password = t_strndup(raw_password, size);
	return strcasecmp(password,
		password_generate_otp(plaintext, password, -1)) == 0;
}

static void
otp_generate(const char *plaintext, const char *user __attr_unused__,
	     const unsigned char **raw_password_r, size_t *size_r)
{
	const char *password;

	password = password_generate_otp(plaintext, NULL, OTP_HASH_SHA1);
	*raw_password_r = (const unsigned char *)password;
	*size_r = strlen(password);
}

static void
skey_generate(const char *plaintext, const char *user __attr_unused__,
	      const unsigned char **raw_password_r, size_t *size_r)
{
	const char *password;

	password = password_generate_otp(plaintext, NULL, OTP_HASH_MD4);
	*raw_password_r = (const unsigned char *)password;
	*size_r = strlen(password);
}

static void
rpa_generate(const char *plaintext, const char *user __attr_unused__,
	     const unsigned char **raw_password_r, size_t *size_r)
{
	unsigned char *digest;

	digest = t_malloc(MD5_RESULTLEN);
	password_generate_rpa(plaintext, digest);

	*raw_password_r = digest;
	*size_r = MD5_RESULTLEN;
}

static const struct password_scheme default_schemes[] = {
	{ "CRYPT", PW_ENCODING_NONE, 0, crypt_verify, crypt_generate },
	{ "MD5", PW_ENCODING_NONE, 0, md5_crypt_verify, md5_crypt_generate },
	{ "MD5-CRYPT", PW_ENCODING_NONE, 0,
	  md5_crypt_verify, md5_crypt_generate },
 	{ "SHA", PW_ENCODING_BASE64, SHA1_RESULTLEN, NULL, sha1_generate },
 	{ "SHA1", PW_ENCODING_BASE64, SHA1_RESULTLEN, NULL, sha1_generate },
	{ "SMD5", PW_ENCODING_BASE64, 0, smd5_verify, smd5_generate },
	{ "SSHA", PW_ENCODING_BASE64, 0, ssha_verify, ssha_generate },
	{ "PLAIN", PW_ENCODING_NONE, 0, NULL, plain_generate },
	{ "CLEARTEXT", PW_ENCODING_NONE, 0, NULL, plain_generate },
	{ "CRAM-MD5", PW_ENCODING_HEX, 0, NULL, cram_md5_generate },
	{ "HMAC-MD5", PW_ENCODING_HEX, CRAM_MD5_CONTEXTLEN,
	  NULL, cram_md5_generate },
	{ "DIGEST-MD5", PW_ENCODING_HEX, MD5_RESULTLEN,
	  NULL, digest_md5_generate },
	{ "PLAIN-MD4", PW_ENCODING_HEX, MD4_RESULTLEN,
	  NULL, plain_md4_generate },
	{ "PLAIN-MD5", PW_ENCODING_HEX, MD5_RESULTLEN,
	  NULL, plain_md5_generate },
	{ "LDAP-MD5", PW_ENCODING_BASE64, MD5_RESULTLEN,
	  NULL, plain_md5_generate },
	{ "LANMAN", PW_ENCODING_HEX, LM_HASH_SIZE, NULL, lm_generate },
	{ "NTLM", PW_ENCODING_HEX, NTLMSSP_HASH_SIZE, NULL, ntlm_generate },
	{ "OTP", PW_ENCODING_NONE, 0, otp_verify, otp_generate },
	{ "SKEY", PW_ENCODING_NONE, 0, otp_verify, skey_generate },
	{ "RPA", PW_ENCODING_HEX, MD5_RESULTLEN, NULL, rpa_generate },

	{ NULL, 0, 0, NULL, NULL }
};

void password_schemes_init(void)
{
	const struct password_scheme *s;
#ifdef HAVE_MODULES
	struct module *mod;
	const char *symbol;
#endif

	i_array_init(&schemes_arr, sizeof(default_schemes) /
		     sizeof(default_schemes[0]) + 4);
	for (s = default_schemes; s->name != NULL; s++)
		array_append(&schemes_arr, s, 1);

#ifdef HAVE_MODULES
	scheme_modules = module_dir_load(AUTH_MODULE_DIR"/password",
					 NULL, FALSE, PACKAGE_VERSION);
	module_dir_init(scheme_modules);
	for (mod = scheme_modules; mod != NULL; mod = mod->next) {
		t_push();
		symbol = t_strconcat(mod->name, "_scheme", NULL);
		s = module_get_symbol(mod, symbol);
		if (s != NULL)
			array_append(&schemes_arr, s, 1);
		t_pop();
	}
#endif

	(void)array_append_space(&schemes_arr);
	schemes = array_idx(&schemes_arr, 0);
}

void password_schemes_deinit(void)
{
#ifdef HAVE_MODULES
	module_dir_unload(&scheme_modules);
#endif

	array_free(&schemes_arr);
	schemes = NULL;
}
