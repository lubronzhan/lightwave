/*
 * Copyright © 2012-2015 VMware, Inc.  All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the “License”); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an “AS IS” BASIS, without
 * warranties or conditions of any kind, EITHER EXPRESS OR IMPLIED.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include "includes.h"

static
DWORD
VmDirBuildDefaultDaclForEntry(
    ACCESS_MASK amAccess,
    PSID    pOwnerSid,
    PCSTR   pszAdminsGroupSid,
    PCSTR   pszDomainAdminsGroupSid,
    PCSTR   pszDomainClientsGroupSid,
    BOOLEAN bAnonymousRead,
    BOOLEAN bServicesDacl,
    PACL *  ppDacl
    );

static
DWORD
VmDirSrvAccessCheckSelf(
    PCSTR        pszNormBindedDn,
    PVDIR_ENTRY pEntry,
    PSECURITY_DESCRIPTOR_ABSOLUTE pSecDescAbs,
    ACCESS_MASK accessDesired,
    ACCESS_MASK *psamGranted
    );

static
DWORD
VmDirSrvAccessCheckEntry(
    PACCESS_TOKEN   pToken,
    PSECURITY_DESCRIPTOR_ABSOLUTE pSecDescAbs,
    ACCESS_MASK     accessDesired,
    ACCESS_MASK *   psamGranted
    );

static
DWORD
_VmDirLoadSecurityDescriptorForEntry(
    PVDIR_ENTRY pEntry,
    PSECURITY_DESCRIPTOR_ABSOLUTE *ppSecDescAbs
    )
{
    DWORD dwError = 0;
    PSECURITY_DESCRIPTOR_ABSOLUTE pSecDescAbs = NULL;
    SECURITY_INFORMATION SecInfoAll = (OWNER_SECURITY_INFORMATION |
                                       GROUP_SECURITY_INFORMATION |
                                       DACL_SECURITY_INFORMATION |
                                       SACL_SECURITY_INFORMATION);
    if (pEntry->pAclCtx == NULL)
    {
        dwError = VmDirAllocateMemory(sizeof(*pEntry->pAclCtx), (PVOID*)&pEntry->pAclCtx);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    if (pEntry->pAclCtx->pSecurityDescriptor == NULL || pEntry->pAclCtx->ulSecDescLength == 0)
    {
        dwError = VmDirGetSecurityDescriptorForEntry(pEntry,
                                                     SecInfoAll,
                                                     &pEntry->pAclCtx->pSecurityDescriptor,
                                                     &pEntry->pAclCtx->ulSecDescLength);
        //
        // Legacy entries might not have a security descriptor. Access will
        // be "manually" checked later on.
        //
        if (dwError == ERROR_NO_SECURITY_DESCRIPTOR)
        {
            VMDIR_LOG_WARNING(VMDIR_LOG_MASK_ALL, "No SD found for %s", pEntry->dn.lberbv.bv_val);
            dwError = 0;
        }
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    if (pEntry->pAclCtx->pSecurityDescriptor != NULL)
    {
        dwError = VmDirSecurityAclSelfRelativeToAbsoluteSD(
                    &pSecDescAbs,
                    pEntry->pAclCtx->pSecurityDescriptor);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    *ppSecDescAbs = pSecDescAbs;

cleanup:
    return dwError;
error:
    goto cleanup;
}

static
DWORD
_VmDirLogFailedAccessCheck(
    PVDIR_ACCESS_INFO pAccessInfo,
    PVDIR_ENTRY pEntry,
    ACCESS_MASK accessDesired
    )
{
    PSTR pszAclString = NULL;
    DWORD dwError = 0;

   dwError = LwNtStatusToWin32Error(RtlAllocateSddlCStringFromSecurityDescriptor(
                                        &pszAclString,
                                        pEntry->pAclCtx->pSecurityDescriptor,
                                        SDDL_REVISION_1,
                                        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION));
    BAIL_ON_VMDIR_ERROR(dwError);

    VMDIR_LOG_ERROR(
        VMDIR_LOG_MASK_ALL,
        "Caller (%s/%s) failed to get 0x%x permission to %s. Object's SD: %s",
        pAccessInfo->pszNormBindedDn,
        pAccessInfo->pszBindedObjectSid,
        accessDesired,
        BERVAL_NORM_VAL(pEntry->dn),
        pszAclString);

cleanup:
    VMDIR_SAFE_FREE_STRINGA(pszAclString);
    return dwError;
error:
    goto cleanup;
}

DWORD
VmDirSrvAccessCheck(
    PVDIR_OPERATION     pOperation,
    PVDIR_ACCESS_INFO   pAccessInfo,
    PVDIR_ENTRY         pEntry,
    ACCESS_MASK         accessDesired
    )
{
    DWORD       dwError = 0;
    ACCESS_MASK samGranted = 0;
    PSECURITY_DESCRIPTOR_ABSOLUTE pSecDescAbs = NULL;
    PVDIR_ENTRY pTargetEntry = pEntry;

    assert(pOperation);
    assert(accessDesired != 0);

    if (pOperation->conn == NULL || pOperation->opType != VDIR_OPERATION_TYPE_EXTERNAL)
    {
        goto cleanup; // Access Allowed
    }

    //
    // If the caller is checking for VMDIR_RIGHT_DS_DELETE_CHILD then pEntry
    // is the child object (the one we actually want to delete). However, we
    // need to check the ACL on the parent, so we re-assign pEntry here.
    // This is only necessary due to the 6.5-compat shim which needs the actual
    // pEntry that's being deleted so we can block certain objects from being
    // deleted.
    //
    if (accessDesired == VMDIR_RIGHT_DS_DELETE_CHILD)
    {
        pEntry = pEntry->pParentEntry;
    }

    dwError = _VmDirLoadSecurityDescriptorForEntry(pEntry, &pSecDescAbs);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (VmDirIsLegacySecurityDescriptor(pEntry->pAclCtx->pSecurityDescriptor, pEntry->pAclCtx->ulSecDescLength))
    {
        dwError = VmDirLegacyAccessCheck(pOperation, pAccessInfo, pTargetEntry, accessDesired);
        BAIL_ON_VMDIR_ERROR(dwError);
        goto cleanup; // Access Allowed
    }
    else
    {
        // Check Access Token in connection
        dwError = VmDirSrvAccessCheckEntry(pAccessInfo->pAccessToken, pSecDescAbs, accessDesired, &samGranted);
        if (!dwError)
        {
            if (samGranted != accessDesired)
            {
                dwError = VMDIR_ERROR_INSUFFICIENT_ACCESS;
                BAIL_ON_VMDIR_ERROR(dwError);
            }
            goto cleanup; // Access Allowed
        }
        else if (VMDIR_ERROR_INSUFFICIENT_ACCESS == dwError)
        {
            // Continue with more ACL check
            dwError = 0;
            samGranted = 0;
        }
    }

    // Otherwise, continue (1) Check whether granted with SELF access right
    dwError = VmDirSrvAccessCheckSelf(
                pAccessInfo->pszNormBindedDn,
                pEntry,
                pSecDescAbs,
                accessDesired,
                &samGranted);
    if (!dwError)
    {
        if (samGranted != accessDesired)
        {
            dwError = VMDIR_ERROR_INSUFFICIENT_ACCESS;
            BAIL_ON_VMDIR_ERROR(dwError);
        }
        goto cleanup; // Access Allowed
    }
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VmDirFreeAbsoluteSecurityDescriptor(&pSecDescAbs);

    return dwError;

error:
    _VmDirLogFailedAccessCheck(pAccessInfo, pEntry, accessDesired);
    goto cleanup;
}

static
DWORD
VmDirSrvAccessCheckSelf(
    PCSTR           pszNormBindedDn,
    PVDIR_ENTRY     pEntry,
    PSECURITY_DESCRIPTOR_ABSOLUTE pSecDescAbs,
    ACCESS_MASK     accessDesired,
    ACCESS_MASK *   psamGranted
    )
{
    DWORD           dwError = ERROR_SUCCESS;
    PACCESS_TOKEN   pWellKnownToken = NULL;

    if (pszNormBindedDn == NULL)
    {
        BAIL_WITH_VMDIR_ERROR(dwError, VMDIR_ERROR_INSUFFICIENT_ACCESS);
    }

    if (IsNullOrEmptyString(pEntry->dn.bvnorm_val))
    {
        dwError = VmDirNormalizeDN( &(pEntry->dn), pEntry->pSchemaCtx);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    // If not self, do not need recreate self token
    if (VmDirStringCompareA(pszNormBindedDn, BERVAL_NORM_VAL(pEntry->dn), TRUE) != 0)
    {
        dwError = VMDIR_ERROR_INSUFFICIENT_ACCESS;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirSrvCreateAccessTokenForWellKnowObject(&pWellKnownToken, VMDIR_SELF_SID);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSrvAccessCheckEntry(pWellKnownToken,
                                       pSecDescAbs,
                                       accessDesired,
                                       psamGranted);
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    if (pWellKnownToken)
    {
        VmDirReleaseAccessToken(&pWellKnownToken);
    }

    return dwError;

error:
    goto cleanup;
}

static
DWORD
VmDirSrvAccessCheckEntry(
    PACCESS_TOKEN   pToken,
    PSECURITY_DESCRIPTOR_ABSOLUTE pSecDescAbs,
    ACCESS_MASK     accessDesired,
    ACCESS_MASK *   psamGranted
    )
{
    DWORD dwError = ERROR_SUCCESS;
    ACCESS_MASK AccessMask = 0;

    // Access check
    if (!VmDirAccessCheck(pSecDescAbs,
                          pToken,
                          accessDesired,
                          0,
                          &gVmDirEntryGenericMapping,
                          &AccessMask,
                          &dwError))
    {
        // VmDirAccessCheck return MS error space.  TODO, need a generic way to handle this.
        if (dwError == ERROR_ACCESS_DENIED)
        {
            dwError = VMDIR_ERROR_INSUFFICIENT_ACCESS;
        }
        BAIL_ON_VMDIR_ERROR(dwError);
    }

cleanup:
    *psamGranted = AccessMask;

    return dwError;

error:
    AccessMask = 0;

    goto cleanup;
}

VOID
VmDirFreeAbsoluteSecurityDescriptor(
    PSECURITY_DESCRIPTOR_ABSOLUTE *ppSecDesc
    )
{
    PSID                            pOwner = NULL;
    PSID                            pGroup = NULL;
    PACL                            pDacl = NULL;
    PACL                            pSacl = NULL;
    BOOLEAN                         bDefaulted = FALSE;
    BOOLEAN                         bPresent = FALSE;
    PSECURITY_DESCRIPTOR_ABSOLUTE   pSecDesc = NULL;

    if (ppSecDesc == NULL || *ppSecDesc == NULL) {
        return;
    }

    pSecDesc = *ppSecDesc;

    VmDirGetOwnerSecurityDescriptor(pSecDesc, &pOwner, &bDefaulted);
    VmDirGetGroupSecurityDescriptor(pSecDesc, &pGroup, &bDefaulted);
    VmDirGetDaclSecurityDescriptor(pSecDesc, &bPresent, &pDacl, &bDefaulted);
    VmDirGetSaclSecurityDescriptor(pSecDesc, &bPresent, &pSacl, &bDefaulted);

    VMDIR_SAFE_FREE_MEMORY(pSecDesc);
    VMDIR_SAFE_FREE_MEMORY(pOwner);
    VMDIR_SAFE_FREE_MEMORY(pGroup);
    VMDIR_SAFE_FREE_MEMORY(pDacl);
    VMDIR_SAFE_FREE_MEMORY(pSacl);

    *ppSecDesc = NULL;
}

DWORD
VmDirSrvCreateSecurityDescriptor(
    ACCESS_MASK amAccess,
    PCSTR pszSystemAdministratorDn,
    PCSTR pszAdminsGroupSid,
    PCSTR pszDomainAdminsGroupSid,
    PCSTR pszDomainClientsGroupSid,
    BOOLEAN bProtectedDacl,
    BOOLEAN bAnonymousRead,
    BOOLEAN bServicesDacl,
    PVMDIR_SECURITY_DESCRIPTOR pSecDesc
    )
{
    DWORD                           dwError = ERROR_SUCCESS;
    PSECURITY_DESCRIPTOR_ABSOLUTE   pSecDescAbs = NULL;
    PACL                            pDacl = NULL;
    PSID                            pOwnerSid = NULL;
    PSID                            pGroupSid = NULL;
    PSECURITY_DESCRIPTOR_RELATIVE   pSecDescRel = NULL;
    ULONG                           ulSecDescLen = 1024;
    SECURITY_INFORMATION            SecInfo = 0;

    // Owner: Administrators
    // Get administrator's PSID
    dwError = VmDirGetObjectSidFromDn(pszSystemAdministratorDn, &pOwnerSid);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAllocateMemory(SECURITY_DESCRIPTOR_ABSOLUTE_MIN_SIZE,
                                                (PVOID*)&pSecDescAbs);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirCreateSecurityDescriptorAbsolute(pSecDescAbs,
                                                SECURITY_DESCRIPTOR_REVISION);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (bProtectedDacl)
    {
        dwError = LwNtStatusToWin32Error(
                    RtlSetSecurityDescriptorControl(
                        pSecDescAbs,
                        SE_DACL_PROTECTED,
                        SE_DACL_PROTECTED));
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    dwError = VmDirSetOwnerSecurityDescriptor(
                 pSecDescAbs,
                 pOwnerSid,
                 FALSE);
    BAIL_ON_VMDIR_ERROR(dwError);

    SecInfo |= OWNER_SECURITY_INFORMATION;

    // BUILD-IN Group Administrators
    dwError = VmDirAllocateSidFromCString(pszAdminsGroupSid, &pGroupSid);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSetGroupSecurityDescriptor(
                 pSecDescAbs,
                 pGroupSid,
                 FALSE);
    BAIL_ON_VMDIR_ERROR(dwError);
    pGroupSid = NULL;

    SecInfo |= GROUP_SECURITY_INFORMATION;

    // DACL
    dwError = VmDirBuildDefaultDaclForEntry(amAccess,
                                            pOwnerSid,
                                            pszAdminsGroupSid,
                                            pszDomainAdminsGroupSid,
                                            pszDomainClientsGroupSid,
                                            bAnonymousRead,
                                            bServicesDacl,
                                            &pDacl);
    BAIL_ON_VMDIR_ERROR(dwError);
    pOwnerSid = NULL;

    dwError = VmDirSetDaclSecurityDescriptor(pSecDescAbs,
                                             TRUE,
                                             pDacl,
                                             FALSE);
    BAIL_ON_VMDIR_ERROR(dwError);
    pDacl = NULL;

    SecInfo |= DACL_SECURITY_INFORMATION;

    if (!VmDirValidSecurityDescriptor(pSecDescAbs))
    {
        dwError = ERROR_INVALID_SECURITY_DESCR;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    do
    {
        VMDIR_SAFE_FREE_MEMORY(pSecDescRel);
        dwError = VmDirAllocateMemory(ulSecDescLen,
                                    (PVOID*)&pSecDescRel);
        BAIL_ON_VMDIR_ERROR(dwError);

        memset(pSecDescRel, 0, ulSecDescLen);

        dwError = VmDirAbsoluteToSelfRelativeSD(pSecDescAbs,
                                               pSecDescRel,
                                               &ulSecDescLen);

        if (ERROR_INSUFFICIENT_BUFFER  == dwError)
        {
            ulSecDescLen *= 2;
        }
        else
        {
            BAIL_ON_VMDIR_ERROR(dwError);
        }

    }
    while((dwError != ERROR_SUCCESS) &&
          (ulSecDescLen <= SECURITY_DESCRIPTOR_RELATIVE_MAX_SIZE));

    if (ulSecDescLen > SECURITY_DESCRIPTOR_RELATIVE_MAX_SIZE)
    {
        dwError = ERROR_INVALID_SECURITY_DESCR;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    pSecDesc->pSecDesc = pSecDescRel;
    pSecDesc->ulSecDesc = ulSecDescLen;
    pSecDesc->SecInfo = SecInfo;

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pDacl);
    VMDIR_SAFE_FREE_MEMORY(pOwnerSid);
    VMDIR_SAFE_FREE_MEMORY(pGroupSid);

    VmDirFreeAbsoluteSecurityDescriptor(&pSecDescAbs);

    return dwError;

error:
    VMDIR_SAFE_FREE_MEMORY(pSecDescRel);

    goto cleanup;
}

DWORD
VmDirGetObjectSidFromDn(
    PCSTR   pszObjectDn,
    PSID *  ppSid
    )
{
    DWORD       dwError = 0;
    PVDIR_ENTRY pEntry = NULL;

    dwError = VmDirSimpleDNToEntry(pszObjectDn, &pEntry);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirGetObjectSidFromEntry(pEntry, NULL, ppSid);
    BAIL_ON_VMDIR_ERROR(dwError);

error:
    if (pEntry)
    {
        VmDirFreeEntry(pEntry);
    }

    return dwError;
}

DWORD
VmDirGetObjectSidFromEntry(
    PVDIR_ENTRY pEntry,
    PSTR *      ppszObjectSid, /* Optional */
    PSID *      ppSid /* Optional */
    )
{
    DWORD           dwError = 0;
    PVDIR_ATTRIBUTE pAttrObjectSid = NULL;

    pAttrObjectSid = VmDirEntryFindAttribute( ATTR_OBJECT_SID, pEntry );
    if (!pAttrObjectSid)
    {
        dwError = ERROR_NO_OBJECTSID_ATTR;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    if (ppszObjectSid)
    {
        dwError = VmDirAllocateStringA((PCSTR)pAttrObjectSid->vals[0].lberbv.bv_val, ppszObjectSid);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    if (ppSid)
    {
        dwError = VmDirAllocateSidFromCString((PCSTR)pAttrObjectSid->vals[0].lberbv.bv_val, ppSid);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

error:
    return dwError;
}

VOID
VmDirAclCtxContentFree(
    PVDIR_ACL_CTX pAclCtx
    )
{
    if (pAclCtx)
    {
        VMDIR_SAFE_FREE_MEMORY(pAclCtx->pSecurityDescriptor);
    }
}

static
DWORD
VmDirBuildDefaultDaclForEntry(
    ACCESS_MASK amAccess,
    PSID    pOwnerSid, // system Administrator SID, at least in our context
    PCSTR   pszAdminsGroupSid,
    PCSTR   pszDomainAdminsGroupSid,
    PCSTR   pszDomainClientsGroupSid,
    BOOLEAN bAnonymousRead,
    BOOLEAN bServicesDacl,
    PACL *  ppDacl
    )
{
    DWORD   dwError = ERROR_SUCCESS;
    DWORD   dwSizeDacl = 0;
    PSID    pBuiltInAdmins = NULL;
    PSID    pDomainAdmins = NULL;
    PSID    pDomainClients = NULL;
    PSID    pSelfSid = NULL;
    PSID    pAnonymousSid = NULL;
    DWORD   dwSidCount = 0;
    PACL    pDacl = NULL;

    assert(pOwnerSid);
    dwSidCount++;

    dwError = VmDirAllocateSidFromCString(VMDIR_SELF_SID, &pSelfSid);
    BAIL_ON_VMDIR_ERROR(dwError);
    dwSidCount++;

    if (bAnonymousRead)
    {
        dwError = VmDirAllocateSidFromCString(VMDIR_ANONYMOUS_LOGON_SID, &pAnonymousSid);
        BAIL_ON_VMDIR_ERROR(dwError);
        dwSidCount++;
    }

    dwError = VmDirAllocateSidFromCString(pszAdminsGroupSid, &pBuiltInAdmins);
    BAIL_ON_VMDIR_ERROR(dwError);
    dwSidCount++;

    dwError = VmDirAllocateSidFromCString(pszDomainAdminsGroupSid, &pDomainAdmins);
    BAIL_ON_VMDIR_ERROR(dwError);
    dwSidCount++;

    dwError = VmDirAllocateSidFromCString(pszDomainClientsGroupSid, &pDomainClients);
    BAIL_ON_VMDIR_ERROR(dwError);
    dwSidCount++;

    dwSizeDacl = ACL_HEADER_SIZE +
        dwSidCount * sizeof(ACCESS_ALLOWED_ACE) +
        VmDirLengthSid(pOwnerSid) +
        VmDirLengthSid(pSelfSid) +
        (bAnonymousRead ? VmDirLengthSid(pAnonymousSid) : 0) +
        VmDirLengthSid(pBuiltInAdmins) +
        VmDirLengthSid(pDomainAdmins) +
        VmDirLengthSid(pDomainClients) -
        dwSidCount * sizeof(ULONG);

    dwError = VmDirAllocateMemory(dwSizeDacl, (PVOID*)&pDacl);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirCreateAcl(pDacl, dwSizeDacl, ACL_REVISION);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAddAccessAllowedAceEx(pDacl,
                                         ACL_REVISION,
                                         OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                         amAccess,
                                         pOwnerSid);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAddAccessAllowedAceEx(pDacl,
                                         ACL_REVISION,
                                         OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                         amAccess,
                                         pBuiltInAdmins);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAddAccessAllowedAceEx(pDacl,
                                         ACL_REVISION,
                                         OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                         amAccess,
                                         pDomainAdmins);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAddAccessAllowedAceEx(pDacl,
                                         ACL_REVISION,
                                         OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                         VMDIR_RIGHT_DS_READ_PROP |
                                            (bServicesDacl ? VMDIR_DCCLIENTS_FULL_ACCESS : 0),
                                         pDomainClients);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirAddAccessAllowedAceEx(pDacl,
                                         ACL_REVISION,
                                         OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                         VMDIR_RIGHT_DS_READ_PROP |
                                         VMDIR_RIGHT_DS_WRITE_PROP,
                                         pSelfSid);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (bAnonymousRead)
    {
        dwError = VmDirAddAccessAllowedAceEx(pDacl,
                                             ACL_REVISION,
                                             OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                             VMDIR_RIGHT_DS_READ_PROP,
                                             pAnonymousSid);
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    *ppDacl = pDacl;

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pSelfSid);
    VMDIR_SAFE_FREE_MEMORY(pAnonymousSid);
    VMDIR_SAFE_FREE_MEMORY(pDomainAdmins);
    VMDIR_SAFE_FREE_MEMORY(pDomainClients);
    VMDIR_SAFE_FREE_MEMORY(pBuiltInAdmins);

    return dwError;

error:
    VMDIR_SAFE_FREE_MEMORY(pDacl);

    goto cleanup;
}

/* Given a targetDN and the current bindedDN (the credentail represents the current user access)
 * return whether such bindedDN has adminRole to perform on the targetDN
 * pOperation is optional (to provide operation context if needed)
 *
 */
DWORD
VmDirSrvAccessCheckIsAdminRole(
   PVDIR_OPERATION      pOperation, /* Optional */
   PCSTR                pszNormTargetDN, /* Mandatory */
   PVDIR_ACCESS_INFO    pAccessInfo, /* Mandatory */
   PBOOLEAN             pbIsAdminRole
   )
{
    DWORD   dwError = ERROR_SUCCESS;
    BOOLEAN bIsAdminRole = FALSE;
    PVDIR_BACKEND_CTX pBECtx = NULL;

    if (pOperation != NULL)
    {
        if (pOperation->opType != VDIR_OPERATION_TYPE_EXTERNAL)
        {
            *pbIsAdminRole = TRUE;
            goto cleanup;
        }

        if ( pOperation->conn->bIsAnonymousBind ) // anonymous bind
        {
            *pbIsAdminRole = FALSE;
            goto cleanup;
        }

        pBECtx = pOperation->pBECtx;
    }

    if (IsNullOrEmptyString(pszNormTargetDN))
    {
        dwError = ERROR_INVALID_PARAMETER;
        BAIL_ON_VMDIR_ERROR(dwError);
    }

    BAIL_ON_INVALID_ACCESSINFO(pAccessInfo, dwError);

    //Check whether bindedDN is member of build-in administrators group
    dwError = VmDirIsBindDnMemberOfSystemDomainAdmins(pBECtx,
                                                      pAccessInfo,
                                                      &bIsAdminRole);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (bIsAdminRole)
    {
        *pbIsAdminRole = TRUE;
        goto cleanup;
    }

    // per PROD2013 requirements, member of domaincontrollergroup gets system admin rights.
    if (gVmdirServerGlobals.bvDCGroupDN.lberbv_val == NULL)
    {
        *pbIsAdminRole = FALSE;
        goto cleanup;
    }

    dwError = VmDirIsDirectMemberOf( pAccessInfo->pszBindedDn,
                                     VDIR_ACCESS_DCGROUP_MEMBER_INFO,
                                     &pAccessInfo->accessRoleBitmap,
                                     &bIsAdminRole);
    BAIL_ON_VMDIR_ERROR(dwError);

    *pbIsAdminRole = bIsAdminRole;

cleanup:

    return dwError;

error:
    *pbIsAdminRole = FALSE;

    goto cleanup;
}

/* Check whether it is a valid accessInfo
 * (i.e.: resulted by doing a successful bind in an operation) */
BOOLEAN
VmDirIsFailedAccessInfo(
    PVDIR_ACCESS_INFO pAccessInfo
    )
{

    BOOLEAN     bIsFaliedAccessPermission = TRUE;


    if ( ! pAccessInfo->pAccessToken )
    {   // internal operation has NULL pAccessToken, yet we granted root privilege
        bIsFaliedAccessPermission = FALSE;
    }
    else
    {   // coming from LDAP protocol, we should have BIND information
        if ( ! IsNullOrEmptyString(pAccessInfo->pszBindedObjectSid)
             &&
             ! IsNullOrEmptyString(pAccessInfo->pszNormBindedDn)
             &&
             ! IsNullOrEmptyString(pAccessInfo->pszBindedDn)
           )
        {
            bIsFaliedAccessPermission = FALSE;
        }
    }

    return bIsFaliedAccessPermission;
}

/*
 * This APIs is called under the assumption that it is already checked
 * to make sure that bindedDn and searched Base Dn are in the same tenant scope
 *
 * Do an internalSearch against the domain object of 'pszBindedDn' using 'SUBTREE scope'
 * The filters is constructed as
 * '&(pszAttrFilterName1=pszAttrFilterVal1)(pszAttrFilterName2=pszAttrFilterVal2)'
 * return the found pEntry
 */
DWORD
VmDirIsBindDnMemberOfSystemDomainAdmins(
    PVDIR_BACKEND_CTX   pBECtx,
    PVDIR_ACCESS_INFO   pAccessInfo,
    PBOOLEAN            pbIsMemberOfAdmins
    )
{
    DWORD               dwError = ERROR_SUCCESS;
    VDIR_OPERATION      SearchOp = {0};
    PVDIR_FILTER        pSearchFilter = NULL;
    BOOLEAN             bIsMemberOfAdmins = FALSE;
    PSTR                pszAdminsGroupSid = NULL;
    PVDIR_BACKEND_CTX   pSearchOpBECtx = NULL;
    PSTR                pszBindedDn = NULL;

    if (pAccessInfo->accessRoleBitmap & VDIR_ACCESS_ADMIN_MEMBER_VALID_INFO)
    {
        *pbIsMemberOfAdmins = (pAccessInfo->accessRoleBitmap & VDIR_ACCESS_IS_ADMIN_MEMBER) != 0;
        goto cleanup;
    }

    pszBindedDn = pAccessInfo->pszBindedDn;
    dwError = VmDirInitStackOperation( &SearchOp,
                                       VDIR_OPERATION_TYPE_INTERNAL,
                                       LDAP_REQ_SEARCH,
                                       NULL );
    BAIL_ON_VMDIR_ERROR(dwError);

    // If there is pBECtx passing in, use explicit BE Context
    // Otherwise, establish a new implicit context
    if (pBECtx && pBECtx->pBEPrivate)
    {
        pSearchOpBECtx = SearchOp.pBECtx;
        SearchOp.pBECtx = pBECtx;
    }
    else
    {
        SearchOp.pBEIF = VmDirBackendSelect(SearchOp.reqDn.lberbv.bv_val);
        assert(SearchOp.pBEIF);
    }

    SearchOp.reqDn.lberbv.bv_val = (PSTR)gVmdirServerGlobals.systemDomainDN.lberbv.bv_val;
    SearchOp.reqDn.lberbv.bv_len = VmDirStringLenA(SearchOp.reqDn.lberbv.bv_val);
    SearchOp.request.searchReq.scope = LDAP_SCOPE_SUBTREE;

    dwError = VmDirGenerateWellknownSid(gVmdirServerGlobals.systemDomainDN.lberbv.bv_val,
                                        VMDIR_DOMAIN_ALIAS_RID_ADMINS,
                                        &pszAdminsGroupSid);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirConcatTwoFilters(SearchOp.pSchemaCtx,
                                    ATTR_OBJECT_SID,
                                    pszAdminsGroupSid,
                                    ATTR_MEMBER,
                                    pszBindedDn,
                                    &pSearchFilter);
    BAIL_ON_VMDIR_ERROR(dwError);

    SearchOp.request.searchReq.filter = pSearchFilter;

    VMDIR_LOG_VERBOSE(VMDIR_LOG_MASK_ALL,
          "VmDirIsBindDnMemberOfSystemDomainAdmins: internal search for dn %s", pszBindedDn);

    dwError = VmDirInternalSearch(&SearchOp);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (SearchOp.internalSearchEntryArray.iSize > 0)
    {
        bIsMemberOfAdmins = TRUE;
    }

    pAccessInfo->accessRoleBitmap |= VDIR_ACCESS_ADMIN_MEMBER_VALID_INFO;
    if (bIsMemberOfAdmins)
    {
        // Set VDIR_ACCESS_IS_ADMIN_MEMBER bit
        pAccessInfo->accessRoleBitmap |= VDIR_ACCESS_IS_ADMIN_MEMBER;
    }

    *pbIsMemberOfAdmins = bIsMemberOfAdmins;

cleanup:

    if ( pSearchOpBECtx )
    {
        SearchOp.pBECtx = pSearchOpBECtx;
    }

    VmDirFreeOperationContent(&SearchOp);
    VMDIR_SAFE_FREE_MEMORY(pszAdminsGroupSid);

    return dwError;

error:
    *pbIsMemberOfAdmins = FALSE;

    goto cleanup;
}
