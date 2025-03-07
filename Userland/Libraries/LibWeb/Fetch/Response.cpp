/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/URLParser.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Fetch/Enums.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Infra/JSON.h>

namespace Web::Fetch {

Response::Response(JS::Realm& realm, NonnullRefPtr<Infrastructure::Response> response)
    : PlatformObject(realm)
    , m_response(move(response))
{
    set_prototype(&Bindings::cached_web_prototype(realm, "Response"));
}

Response::~Response() = default;

void Response::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_headers);
}

// https://fetch.spec.whatwg.org/#concept-body-mime-type
// https://fetch.spec.whatwg.org/#ref-for-concept-header-extract-mime-type%E2%91%A7
Optional<MimeSniff::MimeType> Response::mime_type_impl() const
{
    // Objects including the Body interface mixin need to define an associated MIME type algorithm which takes no arguments and returns failure or a MIME type.
    // A Response object’s MIME type is to return the result of extracting a MIME type from its response’s header list.
    return m_response->header_list()->extract_mime_type();
}

// https://fetch.spec.whatwg.org/#concept-body-body
// https://fetch.spec.whatwg.org/#ref-for-concept-body-body%E2%91%A8
Optional<Infrastructure::Body const&> Response::body_impl() const
{
    // Objects including the Body interface mixin have an associated body (null or a body).
    // A Response object’s body is its response’s body.
    return m_response->body().has_value()
        ? m_response->body().value()
        : Optional<Infrastructure::Body const&> {};
}

// https://fetch.spec.whatwg.org/#concept-body-body
// https://fetch.spec.whatwg.org/#ref-for-concept-body-body%E2%91%A8
Optional<Infrastructure::Body&> Response::body_impl()
{
    // Objects including the Body interface mixin have an associated body (null or a body).
    // A Response object’s body is its response’s body.
    return m_response->body().has_value()
        ? m_response->body().value()
        : Optional<Infrastructure::Body&> {};
}

// https://fetch.spec.whatwg.org/#response-create
JS::NonnullGCPtr<Response> Response::create(NonnullRefPtr<Infrastructure::Response> response, Headers::Guard guard, JS::Realm& realm)
{
    // Copy a NonnullRefPtr to the response's header list before response is being move()'d.
    auto response_reader_list = response->header_list();

    // 1. Let responseObject be a new Response object with realm.
    // 2. Set responseObject’s response to response.
    auto* response_object = realm.heap().allocate<Response>(realm, realm, move(response));

    // 3. Set responseObject’s headers to a new Headers object with realm, whose headers list is response’s headers list and guard is guard.
    response_object->m_headers = realm.heap().allocate<Headers>(realm, realm);
    response_object->m_headers->set_header_list(move(response_reader_list));
    response_object->m_headers->set_guard(guard);

    // 4. Return responseObject.
    return JS::NonnullGCPtr { *response_object };
}

// https://fetch.spec.whatwg.org/#initialize-a-response
WebIDL::ExceptionOr<void> Response::initialize_response(ResponseInit const& init, Optional<Infrastructure::BodyWithType> const& body)
{
    // 1. If init["status"] is not in the range 200 to 599, inclusive, then throw a RangeError.
    if (init.status < 200 || init.status > 599)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Status must be in range 200-599"sv };

    // FIXME: 2. If init["statusText"] does not match the reason-phrase token production, then throw a TypeError.

    // 3. Set response’s response’s status to init["status"].
    m_response->set_status(init.status);

    // 4. Set response’s response’s status message to init["statusText"].
    m_response->set_status_message(TRY_OR_RETURN_OOM(realm(), ByteBuffer::copy(init.status_text.bytes())));

    // 5. If init["headers"] exists, then fill response’s headers with init["headers"].
    if (init.headers.has_value())
        m_headers->fill(*init.headers);

    // 6. If body was given, then:
    if (body.has_value()) {
        // 1. If response’s status is a null body status, then throw a TypeError.
        if (Infrastructure::is_null_body_status(m_response->status()))
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Response with null body status cannot have a body"sv };

        // 2. Set response’s body to body’s body.
        m_response->set_body(body->body);

        // 3. If body’s type is non-null and response’s header list does not contain `Content-Type`, then append (`Content-Type`, body’s type) to response’s header list.
        if (body->type.has_value() && m_response->header_list()->contains("Content-Type"sv.bytes())) {
            auto header = Infrastructure::Header {
                .name = TRY_OR_RETURN_OOM(realm(), ByteBuffer::copy("Content-Type"sv.bytes())),
                .value = TRY_OR_RETURN_OOM(realm(), ByteBuffer::copy(body->type->span())),
            };
            TRY_OR_RETURN_OOM(realm(), m_response->header_list()->append(move(header)));
        }
    }

    return {};
}

// https://fetch.spec.whatwg.org/#dom-response
WebIDL::ExceptionOr<JS::NonnullGCPtr<Response>> Response::construct_impl(JS::Realm& realm, Optional<BodyInit> const& body, ResponseInit const& init)
{
    // Referred to as 'this' in the spec.
    auto response_object = JS::NonnullGCPtr { *realm.heap().allocate<Response>(realm, realm, Infrastructure::Response::create()) };

    // 1. Set this’s response to a new response.
    // NOTE: This is done at the beginning as the 'this' value Response object
    //       cannot exist with a null Infrastructure::Response.

    // 2. Set this’s headers to a new Headers object with this’s relevant Realm, whose header list is this’s response’s header list and guard is "response".
    response_object->m_headers = realm.heap().allocate<Headers>(realm, realm);
    response_object->m_headers->set_header_list(response_object->response()->header_list());
    response_object->m_headers->set_guard(Headers::Guard::Response);

    // 3. Let bodyWithType be null.
    Optional<Infrastructure::BodyWithType> body_with_type;

    // 4. If body is non-null, then set bodyWithType to the result of extracting body.
    if (body.has_value())
        body_with_type = TRY(extract_body(realm, *body));

    // 5. Perform initialize a response given this, init, and bodyWithType.
    TRY(response_object->initialize_response(init, body_with_type));

    return response_object;
}

// https://fetch.spec.whatwg.org/#dom-response-error
JS::NonnullGCPtr<Response> Response::error(JS::VM& vm)
{
    // The static error() method steps are to return the result of creating a Response object, given a new network error, "immutable", and this’s relevant Realm.
    // FIXME: How can we reliably get 'this', i.e. the object the function was called on, in IDL-defined functions?
    return Response::create(Infrastructure::Response::network_error(), Headers::Guard::Immutable, *vm.current_realm());
}

// https://fetch.spec.whatwg.org/#dom-response-redirect
WebIDL::ExceptionOr<JS::NonnullGCPtr<Response>> Response::redirect(JS::VM& vm, String const& url, u16 status)
{
    auto& realm = *vm.current_realm();

    // 1. Let parsedURL be the result of parsing url with current settings object’s API base URL.
    auto api_base_url = HTML::current_settings_object().api_base_url();
    auto parsed_url = URLParser::parse(url, &api_base_url);

    // 2. If parsedURL is failure, then throw a TypeError.
    if (!parsed_url.is_valid())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Redirect URL is not valid"sv };

    // 3. If status is not a redirect status, then throw a RangeError.
    if (!Infrastructure::is_redirect_status(status))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Status must be one of 301, 302, 303, 307, or 308"sv };

    // 4. Let responseObject be the result of creating a Response object, given a new response, "immutable", and this’s relevant Realm.
    // FIXME: How can we reliably get 'this', i.e. the object the function was called on, in IDL-defined functions?
    auto response_object = Response::create(Infrastructure::Response::create(), Headers::Guard::Immutable, realm);

    // 5. Set responseObject’s response’s status to status.
    response_object->response()->set_status(status);

    // 6. Let value be parsedURL, serialized and isomorphic encoded.
    auto value = parsed_url.serialize();

    // 7. Append (`Location`, value) to responseObject’s response’s header list.
    auto header = Infrastructure::Header {
        .name = TRY_OR_RETURN_OOM(realm, ByteBuffer::copy("Location"sv.bytes())),
        .value = TRY_OR_RETURN_OOM(realm, ByteBuffer::copy(value.bytes())),
    };
    TRY_OR_RETURN_OOM(realm, response_object->response()->header_list()->append(move(header)));

    // 8. Return responseObject.
    return response_object;
}

// https://fetch.spec.whatwg.org/#dom-response-json
WebIDL::ExceptionOr<JS::NonnullGCPtr<Response>> Response::json(JS::VM& vm, JS::Value data, ResponseInit const& init)
{
    auto& realm = *vm.current_realm();

    // 1. Let bytes the result of running serialize a JavaScript value to JSON bytes on data.
    auto bytes = TRY(Infra::serialize_javascript_value_to_json_bytes(vm, data));

    // 2. Let body be the result of extracting bytes.
    auto [body, _] = TRY(extract_body(realm, { bytes.bytes() }));

    // 3. Let responseObject be the result of creating a Response object, given a new response, "response", and this’s relevant Realm.
    // FIXME: How can we reliably get 'this', i.e. the object the function was called on, in IDL-defined functions?
    auto response_object = Response::create(Infrastructure::Response::create(), Headers::Guard::Response, realm);

    // 4. Perform initialize a response given responseObject, init, and (body, "application/json").
    auto body_with_type = Infrastructure::BodyWithType {
        .body = move(body),
        .type = TRY_OR_RETURN_OOM(realm, ByteBuffer::copy("application/json"sv.bytes()))
    };
    TRY(response_object->initialize_response(init, move(body_with_type)));

    // 5. Return responseObject.
    return response_object;
}

// https://fetch.spec.whatwg.org/#dom-response-type
Bindings::ResponseType Response::type() const
{
    // The type getter steps are to return this’s response’s type.
    return to_bindings_enum(m_response->type());
}

// https://fetch.spec.whatwg.org/#dom-response-url
String Response::url() const
{
    // The url getter steps are to return the empty string if this’s response’s URL is null; otherwise this’s response’s URL, serialized with exclude fragment set to true.
    return !m_response->url().has_value()
        ? String::empty()
        : m_response->url()->serialize(AK::URL::ExcludeFragment::Yes);
}

// https://fetch.spec.whatwg.org/#dom-response-redirected
bool Response::redirected() const
{
    // The redirected getter steps are to return true if this’s response’s URL list has more than one item; otherwise false.
    return m_response->url_list().size() > 1;
}

// https://fetch.spec.whatwg.org/#dom-response-status
u16 Response::status() const
{
    // The status getter steps are to return this’s response’s status.
    return m_response->status();
}

// https://fetch.spec.whatwg.org/#dom-response-ok
bool Response::ok() const
{
    // The ok getter steps are to return true if this’s response’s status is an ok status; otherwise false.
    return Infrastructure::is_ok_status(m_response->status());
}

// https://fetch.spec.whatwg.org/#dom-response-statustext
String Response::status_text() const
{
    // The statusText getter steps are to return this’s response’s status message.
    return String::copy(m_response->status_message());
}

// https://fetch.spec.whatwg.org/#dom-response-headers
JS::NonnullGCPtr<Headers> Response::headers() const
{
    // The headers getter steps are to return this’s headers.
    return *m_headers;
}

// https://fetch.spec.whatwg.org/#dom-response-clone
WebIDL::ExceptionOr<JS::NonnullGCPtr<Response>> Response::clone() const
{
    // 1. If this is unusable, then throw a TypeError.
    if (is_unusable())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Response is unusable"sv };

    // 2. Let clonedResponse be the result of cloning this’s response.
    auto cloned_response = TRY(m_response->clone());

    // 3. Return the result of creating a Response object, given clonedResponse, this’s headers’s guard, and this’s relevant Realm.
    return Response::create(move(cloned_response), m_headers->guard(), HTML::relevant_realm(*this));
}

}
