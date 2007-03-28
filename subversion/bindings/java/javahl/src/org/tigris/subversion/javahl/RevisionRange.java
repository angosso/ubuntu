/**
 * @copyright
 * ====================================================================
 * Copyright (c) 2007 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 * @endcopyright
 */

package org.tigris.subversion.javahl;

/**
 * Object that describes a revision range
 * 
 * 
 * @since 1.5
 */
public class RevisionRange implements Comparable
{

    private Revision from;
    private Revision to;

    public RevisionRange(Revision from, Revision to)
    {
        super();
        this.from = from;
        this.to = to;
    }

    public Revision getFromRevision()
    {
        return from;
    }

    public Revision getToRevision()
    {
        return to;
    }

    public String toString()
    {
        if (from != null && to != null)
        {
            if (from.equals(to))
                return from.toString();
            else
                return from.toString() + '-' + to.toString();
        }
        return super.toString();
    }

    public static Long getRevisionAsLong(Revision rev)
    {
        long val = 0;
        if (rev != null && rev instanceof Revision.Number)
        {
            val = ((Revision.Number) rev).getNumber();
        }
        return new Long(val);
    }

    public int hashCode()
    {
        final int prime = 31;
        int result = 1;
        result = prime * result + ((from == null) ? 0 : from.hashCode());
        result = prime * result + ((to == null) ? 0 : to.hashCode());
        return result;
    }

    /**
     * @param range The RevisionRange to compare this object to.
     */
    public boolean equals(Object range)
    {
        if (this == range)
            return true;
        if (!super.equals(range))
            return false;
        if (getClass() != range.getClass())
            return false;

        final RevisionRange other = (RevisionRange) range;

        if (from == null)
        {
            if (other.from != null)
                return false;
        }
        else if (!from.equals(other.from))
        {
            return false;
        }

        if (to == null)
        {
            if (other.to != null)
                return false;
        }
        else if (!to.equals(other.to))
        {
            return false;
        }

        return true;
    }

    /**
     * @param range The RevisionRange to compare this object to.
     */
    public int compareTo(Object range)
    {
        if (this == range)
            return 0;

        Revision other = ((RevisionRange) range).getFromRevision();
        return RevisionRange.getRevisionAsLong(this.getFromRevision())
            .compareTo(RevisionRange.getRevisionAsLong(other));
    }
}
