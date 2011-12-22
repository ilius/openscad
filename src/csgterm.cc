/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright (C) 2009-2011 Clifford Wolf <clifford@clifford.at> and
 *                          Marius Kintel <marius@kintel.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  As a special exception, you have permission to link this program
 *  with the CGAL library and distribute executables, as long as you
 *  follow the requirements of the GNU GPL in regard to all of the
 *  software in the executable aside from CGAL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "csgterm.h"
#include "polyset.h"
#include "linalg.h"
#include <sstream>

/*!
	\class CSGTerm

	A CSGTerm is either a "primitive" or a CSG operation with two
	children terms. A primitive in this context is any PolySet, which
	may or may not have a subtree which is already evaluated (e.g. using
	the render() module).

 */

/*!
	\class CSGChain

	A CSGChain is just a vector of primitives, each having a CSG type associated with it.
	It's created by importing a CSGTerm tree.

 */


shared_ptr<CSGTerm> CSGTerm::createCSGTerm(type_e type, shared_ptr<CSGTerm> left, shared_ptr<CSGTerm> right)
{
	if (type != TYPE_PRIMITIVE) {
		// In case we're creating a CSG terms from a pruned tree, left/right can be NULL
		if (!right) {
			if (type == TYPE_UNION || type == TYPE_DIFFERENCE) return left;
			else return right;
		}
		if (!left) {
			if (type == TYPE_UNION) return right;
			else return left;
		}
	}

  // Pruning the tree. For details, see:
  // http://www.cc.gatech.edu/~turk/my_papers/pxpl_csg.pdf
	const BoundingBox &leftbox = left->getBoundingBox();
	const BoundingBox &rightbox = right->getBoundingBox();
	if (type == TYPE_INTERSECTION) {
		BoundingBox newbox(leftbox.min().cwise().max(rightbox.min()),
											 leftbox.max().cwise().min(rightbox.max()));
		if (newbox.isNull()) {
			return shared_ptr<CSGTerm>(); // Prune entire product
		}
	}
	else if (type == TYPE_DIFFERENCE) {
		BoundingBox newbox(leftbox.min().cwise().max(rightbox.min()),
											 leftbox.max().cwise().min(rightbox.max()));
		if (newbox.isNull()) {
			return left; // Prune the negative component
		}
	}

	return shared_ptr<CSGTerm>(new CSGTerm(type, left, right));
}

shared_ptr<CSGTerm> CSGTerm::createCSGTerm(type_e type, CSGTerm *left, CSGTerm *right)
{
	return createCSGTerm(type, shared_ptr<CSGTerm>(left), shared_ptr<CSGTerm>(right));
}

CSGTerm::CSGTerm(const shared_ptr<PolySet> &polyset, const Transform3d &matrix, const double color[4], const std::string &label)
	: type(TYPE_PRIMITIVE), polyset(polyset), label(label), m(matrix)
{
	for (int i = 0; i < 4; i++) this->color[i] = color[i];
	initBoundingBox();
}

CSGTerm::CSGTerm(type_e type, shared_ptr<CSGTerm> left, shared_ptr<CSGTerm> right)
	: type(type), left(left), right(right), m(Transform3d::Identity())
{
	initBoundingBox();
}

CSGTerm::CSGTerm(type_e type, CSGTerm *left, CSGTerm *right)
	: type(type), left(left), right(right), m(Transform3d::Identity())
{
	initBoundingBox();
}

CSGTerm::~CSGTerm()
{
}

void CSGTerm::initBoundingBox()
{
	if (this->type == TYPE_PRIMITIVE) {
		BoundingBox polybox = this->polyset->getBoundingBox();
		this->bbox.extend(this->m * polybox.min());
		this->bbox.extend(this->m * polybox.max());
	}
	else {
		const BoundingBox &leftbox = this->left->getBoundingBox();
		const BoundingBox &rightbox = this->right->getBoundingBox();
		switch (this->type) {
		case TYPE_UNION:
			this->bbox.extend(this->m * leftbox.min().cwise().min(rightbox.min()));
			this->bbox.extend(this->m * leftbox.max().cwise().max(rightbox.max()));
			break;
		case TYPE_INTERSECTION:
			this->bbox.extend(this->m * leftbox.min().cwise().max(rightbox.min()));
			this->bbox.extend(this->m * leftbox.max().cwise().min(rightbox.max()));
			break;
		case TYPE_DIFFERENCE:
			this->bbox.extend(this->m * leftbox.min());
			this->bbox.extend(this->m * leftbox.max());
			break;
		case TYPE_PRIMITIVE:
			break;
		default:
			assert(false);
		}
	}
}

shared_ptr<CSGTerm> CSGTerm::normalize(shared_ptr<CSGTerm> term)
{
	// This function implements the CSG normalization
  // Reference:
	// Goldfeather, J., Molnar, S., Turk, G., and Fuchs, H. Near
	// Realtime CSG Rendering Using Tree Normalization and Geometric
	// Pruning. IEEE Computer Graphics and Applications, 9(3):20-28,
	// 1989.
  // http://www.cc.gatech.edu/~turk/my_papers/pxpl_csg.pdf

	if (term->type == TYPE_PRIMITIVE) {
		return term;
	}

	do {
		while (term && normalize_tail(term)) { }
		if (!term || term->type == TYPE_PRIMITIVE) return term;
		term->left = normalize(term->left);
	} while (term->type != TYPE_UNION &&
					 (term->right->type != TYPE_PRIMITIVE || term->left->type == TYPE_UNION));
	term->right = normalize(term->right);

	// FIXME: Do we need to take into account any transformation of item here?
	if (!term->right) {
		if (term->type == TYPE_UNION || term->type == TYPE_DIFFERENCE) return term->left;
		else return term->right;
	}
	if (!term->left) {
		if (term->type == TYPE_UNION) return term->right;
		else return term->left;
	}

	return term;
}

bool CSGTerm::normalize_tail(shared_ptr<CSGTerm> &term)
{
	if (term->type == TYPE_UNION || term->type == TYPE_PRIMITIVE) return false;

	// Part A: The 'x . (y . z)' expressions

	shared_ptr<CSGTerm> x = term->left;
	shared_ptr<CSGTerm> y = term->right->left;
	shared_ptr<CSGTerm> z = term->right->right;

	shared_ptr<CSGTerm> result = term;

	// 1.  x - (y + z) -> (x - y) - z
	if (term->type == TYPE_DIFFERENCE && term->right->type == TYPE_UNION) {
		term = createCSGTerm(TYPE_DIFFERENCE, 
												 createCSGTerm(TYPE_DIFFERENCE, x, y),
												 z);
		return true;
	}
	// 2.  x * (y + z) -> (x * y) + (x * z)
	else if (term->type == TYPE_INTERSECTION && term->right->type == TYPE_UNION) {
		term = createCSGTerm(TYPE_UNION, 
												 createCSGTerm(TYPE_INTERSECTION, x, y), 
												 createCSGTerm(TYPE_INTERSECTION, x, z));
		return true;
	}
	// 3.  x - (y * z) -> (x - y) + (x - z)
	else if (term->type == TYPE_DIFFERENCE && term->right->type == TYPE_INTERSECTION) {
		term = createCSGTerm(TYPE_UNION, 
												 createCSGTerm(TYPE_DIFFERENCE, x, y), 
												 createCSGTerm(TYPE_DIFFERENCE, x, z));
		return true;
	}
	// 4.  x * (y * z) -> (x * y) * z
	else if (term->type == TYPE_INTERSECTION && term->right->type == TYPE_INTERSECTION) {
		term = createCSGTerm(TYPE_INTERSECTION, 
												 createCSGTerm(TYPE_INTERSECTION, x, y),
												 z);
		return true;
	}
	// 5.  x - (y - z) -> (x - y) + (x * z)
	else if (term->type == TYPE_DIFFERENCE && term->right->type == TYPE_DIFFERENCE) {
		term = createCSGTerm(TYPE_UNION, 
												 createCSGTerm(TYPE_DIFFERENCE, x, y), 
												 createCSGTerm(TYPE_INTERSECTION, x, z));
		return true;
	}
	// 6.  x * (y - z) -> (x * y) - z
	else if (term->type == TYPE_INTERSECTION && term->right->type == TYPE_DIFFERENCE) {
		term = createCSGTerm(TYPE_DIFFERENCE, 
												 createCSGTerm(TYPE_INTERSECTION, x, y),
												 z);
		return true;
	}

	// Part B: The '(x . y) . z' expressions

	x = term->left->left;
	y = term->left->right;
	z = term->right;

	// 7. (x - y) * z  -> (x * z) - y
	if (term->left->type == TYPE_DIFFERENCE && term->type == TYPE_INTERSECTION) {
		term = createCSGTerm(TYPE_DIFFERENCE, 
												 createCSGTerm(TYPE_INTERSECTION, x, z), 
												 y);
		return true;
	}
	// 8. (x + y) - z  -> (x - z) + (y - z)
	else if (term->left->type == TYPE_UNION && term->type == TYPE_DIFFERENCE) {
		term = createCSGTerm(TYPE_UNION, 
												 createCSGTerm(TYPE_DIFFERENCE, x, z), 
												 createCSGTerm(TYPE_DIFFERENCE, y, z));
		return true;
	}
	// 9. (x + y) * z  -> (x * z) + (y * z)
	else if (term->left->type == TYPE_UNION && term->type == TYPE_INTERSECTION) {
		term = createCSGTerm(TYPE_UNION, 
												 createCSGTerm(TYPE_INTERSECTION, x, z), 
												 createCSGTerm(TYPE_INTERSECTION, y, z));
		return true;
	}

	return false;
}

std::string CSGTerm::dump()
{
	std::stringstream dump;

	if (type == TYPE_UNION)
		dump << "(" << left->dump() << " + " << right->dump() << ")";
	else if (type == TYPE_INTERSECTION)
		dump << "(" << left->dump() << " * " << right->dump() << ")";
	else if (type == TYPE_DIFFERENCE)
		dump << "(" << left->dump() << " - " << right->dump() << ")";
	else 
		dump << this->label;

	return dump.str();
}

CSGChain::CSGChain()
{
}

void CSGChain::add(const shared_ptr<PolySet> &polyset, const Transform3d &m, double *color, CSGTerm::type_e type, std::string label)
{
	polysets.push_back(polyset);
	matrices.push_back(m);
	colors.push_back(color);
	types.push_back(type);
	labels.push_back(label);
}

void CSGChain::import(shared_ptr<CSGTerm> term, CSGTerm::type_e type)
{
	if (term->type == CSGTerm::TYPE_PRIMITIVE) {
		add(term->polyset, term->m, term->color, type, term->label);
	} else {
		import(term->left, type);
		import(term->right, term->type);
	}
}

std::string CSGChain::dump()
{
	std::stringstream dump;

	for (size_t i = 0; i < types.size(); i++)
	{
		if (types[i] == CSGTerm::TYPE_UNION) {
			if (i != 0) dump << "\n";
			dump << "+";
		}
		else if (types[i] == CSGTerm::TYPE_DIFFERENCE)
			dump << " -";
		else if (types[i] == CSGTerm::TYPE_INTERSECTION)
			dump << " *";
		dump << labels[i];
	}
	dump << "\n";
	return dump.str();
}

BoundingBox CSGChain::getBoundingBox() const
{
	BoundingBox bbox;
	for (size_t i=0;i<polysets.size();i++) {
		if (types[i] != CSGTerm::TYPE_DIFFERENCE) {
			BoundingBox psbox = polysets[i]->getBoundingBox();
			if (!psbox.isNull()) {
				Eigen::Transform3d t;
				// Column-major vs. Row-major
				t = matrices[i];
				bbox.extend(t * psbox.min());
				bbox.extend(t * psbox.max());
			}
		}
	}
	return bbox;
}
